// MobileGL - separate-process server and TCP session supervisor.
// SPDX-License-Identifier: LGPL-3.0-only
#include "../Transport/Doorbell.h"
#include "../Transport/FdPassing.h"
#include "../Transport/SocketTransport.h"
#include "../Transport/WireLog.h"
#include "../Protocol/SurfaceOpCodec.h"
#include "../Handshake.h"
#include "../FatalFunnel.h"
#include "PreAuthGate.h"
#include "ServerLoop.h"
#include "ServerSession.h"
#include "SurfaceControlFrame.h"
#include <Config.h>
#include <Init.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Backend/ServerRole.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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

// The single-session TCP shape's wait for its data connection's DataBind (ListenerSource).
constexpr std::uint32_t kFirstFrameWaitMs = 2000;
constexpr std::uint64_t kFirstFrameMaxBytes = Server::kPreAuthFirstFrameMaxBytes;
// The one detail both ends of the hand-off use for a data connection the live session will not
// take: the supervisor when the send fails, and the child for the ones queued before it closed
// its end (CloseHandoffRefusingQueued).
constexpr const char* kHandoffRefused = "data connection could not be handed to the live session";

// Whatever the peer has already sent, read and dropped (non-blocking, at most 1 MiB), so a close
// with bytes still unread does not become an RST that discards the refusal in flight - the
// reason RefuseBusy reads the Hello before refusing.
void DrainUnread(int fd) {
    char sink[4096];
    for (int rounds = 0; rounds < 256; ++rounds) {
        if (::recv(fd, sink, sizeof(sink), MSG_DONTWAIT) <= 0) return;
    }
}

// Refuses a connection the supervisor still holds as a bare descriptor, and closes it.
void RefuseAndClose(int fd, Protocol::RefuseCode code, const char* detail) {
    SocketTransport connection(fd, -1, TransportRole::Server);
    DrainUnread(fd);
    Refuse(connection, code, detail);
}

// The numeric address of a TCP peer, without its port: the backoff's key. A peer whose address
// cannot be read is keyed "?", so it is still counted, just not on its own.
std::string PeerAddress(int fd) {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) return "?";
    char text[INET6_ADDRSTRLEN] = {};
    const void* raw = nullptr;
    if (address.ss_family == AF_INET) raw = &reinterpret_cast<const sockaddr_in*>(&address)->sin_addr;
    else if (address.ss_family == AF_INET6) raw = &reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr;
    if (raw == nullptr || ::inet_ntop(address.ss_family, raw, text, sizeof(text)) == nullptr) return "?";
    return text;
}

// True when the control peer has closed (EOF or error); false while it is open, readable or not.
bool ControlPeerGone(int controlFd) {
    char byte = 0;
    const auto peeked = ::recv(controlFd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return peeked == 0 || (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

// The session child's source: descriptors the supervisor forwarded, each with the nonce its
// DataBind presented. Watching the control connection too, so a client that went away after
// Welcome ends the wait at once instead of holding the supervisor Busy for the whole deadline.
Server::ServerSession::DataConnectionSource HandoffSource(int handoff, int controlFd) {
    return [handoff, controlFd](std::uint32_t timeoutMs, int* outFd, std::uint8_t* outNonce) -> MobileGLResult {
        pollfd fds[2] = {{handoff, POLLIN, 0}, {controlFd, POLLIN, 0}};
        // A control connection with bytes waiting is not a hang-up and must not spin this loop:
        // after the first look it is only asked again once per slice.
        const int rc = ::poll(fds, 2, static_cast<int>(std::min<std::uint32_t>(timeoutMs, 50)));
        if (rc < 0) return errno == EINTR ? MOBILEGL_ERR_TIMEOUT : MOBILEGL_ERR_TRANSPORT_CLOSED;
        if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && ControlPeerGone(controlFd))
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            if ((fds[1].revents & POLLIN) != 0) {
                pollfd only{handoff, POLLIN, 0};
                if (::poll(&only, 1, static_cast<int>(std::min<std::uint32_t>(timeoutMs, 50))) <= 0)
                    return MOBILEGL_ERR_TIMEOUT;
            } else {
                return MOBILEGL_ERR_TIMEOUT;
            }
        }
        std::uint8_t sideband[FdPassing::kMaxSidebandBytes] = {};
        std::uint64_t size = 0;
        const auto received = FdPassing::ReceiveFd(handoff, outFd, MobileGLMutableByteSpan{sideband, sizeof(sideband)},
                                                   &size, 0);
        if (received != MOBILEGL_OK) return received;
        // The supervisor always sends exactly the presented nonce; a short sideband compares as
        // zeros, which no minted nonce is going to equal by accident.
        std::memcpy(outNonce, sideband, kDataNonceBytes);
        return MOBILEGL_OK;
    };
}

// The single-session (no --serve) source: this process still owns the listener, so it accepts
// the data connection itself and reads its DataBind the same way the supervisor would.
Server::ServerSession::DataConnectionSource ListenerSource(int listener, int controlFd) {
    return [listener, controlFd](std::uint32_t timeoutMs, int* outFd, std::uint8_t* outNonce) -> MobileGLResult {
        if (ControlPeerGone(controlFd)) return MOBILEGL_ERR_TRANSPORT_CLOSED;
        int fd = -1;
        const auto accepted = SocketTransport::AcceptOne(listener, std::min<std::uint32_t>(timeoutMs, 250), &fd);
        if (accepted != MOBILEGL_OK) return accepted;
        std::vector<std::uint8_t> frame;
        std::uint8_t presented[kDataNonceBytes] = {};
        const auto read = SocketTransport::ReceiveOneFrame(fd, kFirstFrameWaitMs, kFirstFrameMaxBytes, &frame);
        if (read != MOBILEGL_OK || !DecodeDataBind(frame, presented)) {
            SocketTransport stray(fd, -1, TransportRole::Server);
            Refuse(stray, Protocol::RefuseCode::Authentication, "data connection's first frame is not a DataBind");
            return MOBILEGL_ERR_TIMEOUT;
        }
        std::memcpy(outNonce, presented, kDataNonceBytes);
        *outFd = fd;
        return MOBILEGL_OK;
    };
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

// What `sourceFd` is, so RunSession knows how to let go of it (ID-P7-44).
enum class DataSource { None, Handoff, Listener };

// ID-P7-44. THE HAND-OFF CLOSES WITHOUT A WINDOW.
//
// The F fix round closed the child's end of the hand-off the moment Accept returned, so that a
// DataBind arriving after the session bound fails to send and the supervisor refuses it by name.
// Between BindDataConnection taking the matching descriptor and that close, though, the
// supervisor could still queue one more - and a close with datagrams queued drops their
// descriptors in the kernel: the peer saw its connection end with no frame at all. So the end is
// cut for reading FIRST (after shutdown(SHUT_RD) every further sendmsg to it fails with EPIPE, which
// FdPassing reports as "peer gone" and the supervisor answers by name), and then whatever was
// queued before the cut is received here and refused with the supervisor's own words. No
// DataBind can be in neither place - on the way out after Accept here, and on every way out
// before it through ExitBeforeAccept below; only a crashed child still drops them.
void CloseHandoffRefusingQueued(int handoff) {
    (void)::shutdown(handoff, SHUT_RD);
    for (;;) {
        int fd = -1;
        std::uint8_t sideband[FdPassing::kMaxSidebandBytes] = {};
        std::uint64_t size = 0;
        if (FdPassing::ReceiveFd(handoff, &fd, MobileGLMutableByteSpan{sideband, sizeof(sideband)}, &size, 0) !=
                MOBILEGL_OK ||
            fd < 0)
            break;
        RefuseAndClose(fd, Protocol::RefuseCode::Authentication, kHandoffRefused);
    }
    ::close(handoff);
}

// ID-P7-44, f2-auth fix round. EVERY WAY OUT OF A SESSION CHILD BEFORE ACCEPT LETS GO OF THE
// HAND-OFF THE WAY THE WAY OUT AFTER IT DOES. The supervisor forwards DataBinds to a child from the
// moment it forks it, so a child that refused its Hello at ValidatePeerHandshake, refused its
// backend, or could not bring one up used to `_exit` with DataBinds queued on the hand-off - and an
// exit drops queued descriptors exactly as a close does: those peers saw their connections end
// with no frame. On any other source (the single-session listener, the unix endpoint's none) there
// is nothing queued to refuse, and this is the `fflush` + `_exit` it replaces.
[[noreturn]] void ExitBeforeAccept(int sourceFd, DataSource sourceKind, int code) {
    if (sourceFd >= 0 && sourceKind == DataSource::Handoff) CloseHandoffRefusingQueued(sourceFd);
    std::fflush(nullptr);
    ::_exit(code);
}

// A TEST-ONLY lever (f2-auth fix round, ID-P7-44): MOBILEGL_TEST_DELAY_SESSION_HANDSHAKE_MS holds a
// session child for that many milliseconds before it looks at its Hello, so a control can queue
// DataBinds on the hand-off of a child that is about to refuse that Hello - a window of
// microseconds otherwise. Like MOBILEGL_TEST_DELAY_FIRST_CAPS_MS (ServerSession.cpp) it exists only
// in the disaggregated server, does nothing unless set, and says so when it is.
void DelaySessionHandshakeForTest() {
    const char* text = std::getenv("MOBILEGL_TEST_DELAY_SESSION_HANDSHAKE_MS");
    if (text == nullptr || *text == '\0') return;
    const long ms = std::strtol(text, nullptr, 10);
    if (ms <= 0) return;
    WireLogError("MG_Remote server: pid=%d MOBILEGL_TEST_DELAY_SESSION_HANDSHAKE_MS=%ld - this session child "
                 "waits that long before its handshake. A test-only lever; never set it in a measured run",
                 static_cast<int>(::getpid()), ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long>(ms, 60000)));
}

// Called only after fork (or, in the single-session shape, instead of it): the supervisor never
// creates a backend or EGL context. `dataSource` is set on TCP (PH-7 (4)) and empty on a unix
// endpoint, whose second connection is the SCM_RIGHTS socket AcceptPair already paired.
// `sourceFd` is the descriptor that source reads from - the child's end of the hand-off
// socketpair under --serve, the listener itself in the single-session shape - and -1 when there
// is none; it is let go of the moment Accept returns, and on every exit before that
// (ExitBeforeAccept, ID-P7-44).
//
// `helloFrame` is the first frame when somebody has already read and authenticated it - the TCP
// supervisor always has (PH-7 (5)) - and empty when this function reads it itself, within
// `helloWaitMs`.
[[noreturn]] void RunSession(std::unique_ptr<SocketTransport> control, std::vector<std::uint8_t> helloFrame,
                             Server::ServerSession::DataConnectionSource dataSource, int sourceFd,
                             DataSource sourceKind, std::uint32_t helloWaitMs) {
    const int selfPid = static_cast<int>(::getpid());
    const bool tcp = control->IsTcp();
    if (helloFrame.empty()) {
        // Every way this read can go wrong is the PEER's (silence, a stream that is not ours, a
        // close), so each is answered by name where the peer is still there to read it and is a
        // clean exit rather than a counted fault (PH-7 (5)).
        std::uint64_t bytes = 0;
        const auto received = control->ReceiveFrame({nullptr, 0}, &bytes, helloWaitMs);
        if (received == MOBILEGL_ERR_TIMEOUT) {
            Refuse(*control, Protocol::RefuseCode::Authentication, Server::kPreAuthDeadline);
            ExitBeforeAccept(sourceFd, sourceKind, 0);
        }
        if (received == MOBILEGL_ERR_PROTOCOL_MISMATCH ||
            (received == MOBILEGL_ERR_BUFFER_TOO_SMALL && bytes > Server::kPreAuthFirstFrameMaxBytes)) {
            DrainUnread(control->StreamFd());
            Refuse(*control, Protocol::RefuseCode::MalformedHello, Server::kFirstFrameNotAFrame);
            ExitBeforeAccept(sourceFd, sourceKind, 0);
        }
        if (received == MOBILEGL_ERR_TRANSPORT_CLOSED) {
            // Fix round: this used to be a bare `_exit(0)` whatever had arrived. A close INSIDE
            // the frame is answered by name, as the TCP supervisor answers it (the peer may have
            // closed only its write side); a close before any byte has nobody to answer, and the
            // log says which of the two it was.
            if (control->BufferedBytes() != 0)
                Refuse(*control, Protocol::RefuseCode::MalformedHello, Server::kFirstFrameTruncated);
            else
                WireLogError("MG_Remote server: pid=%d control peer closed before sending a first frame", selfPid);
            ExitBeforeAccept(sourceFd, sourceKind, 0);
        }
        if (received == MOBILEGL_ERR_BUFFER_TOO_SMALL) {
            // The reassembler holds the whole frame already; this second call copies it out.
            helloFrame.resize(static_cast<std::size_t>(bytes));
            if (control->ReceiveFrame({helloFrame.data(), helloFrame.size()}, &bytes, 0) != MOBILEGL_OK) {
                WireLogError("MG_Remote server: pid=%d first frame of %llu bytes could not be copied out", selfPid,
                             static_cast<unsigned long long>(helloFrame.size()));
                ExitBeforeAccept(sourceFd, sourceKind, 0);
            }
        } else if (received != MOBILEGL_OK) {
            WireLogError("MG_Remote server: pid=%d first frame could not be read (rc=%d)", selfPid,
                         static_cast<int>(received));
            ExitBeforeAccept(sourceFd, sourceKind, 0);
        }
        // MOBILEGL_OK with nothing copied is a ZERO-LENGTH frame. It was the one complete frame
        // this read exited on without a word (fix round); `helloFrame` stays empty and the
        // classification below names it, as the TCP supervisor's does ("no identifier").
    }
    DelaySessionHandshakeForTest();
    // THE FIRST FRAME, CLASSIFIED BY THE SAME FUNCTION THE TCP SUPERVISOR USES (plan §1.1, fuzz
    // arm 1 row: "ServerMain.cpp:193 CtrlEnvelopeBufferHasIdentifier"). The identifier is asked
    // first, and every malformed shape is refused BY NAME - this read used to `_exit(67)` on all
    // of them, so a peer that sent garbage got a bare close and the supervisor counted a fault.
    const auto shape = Server::ClassifyFirstFrame(helloFrame.data(), helloFrame.size());
    // PH-7 (4). A DataBind with no live session to join: the supervisor routes a data connection
    // to a child only while one is live, so this is a stale connection (its session already
    // ended) or one that never had a session. Refused by name, and the child is a clean exit -
    // it is a refusal, not a fault. (On TCP --serve the supervisor answers this itself now; this
    // arm is the single-session shape's.)
    if (shape == Server::FirstFrameShape::DataBind) {
        Refuse(*control, Protocol::RefuseCode::Authentication, "data connection names no live session");
        ExitBeforeAccept(sourceFd, sourceKind, 0);
    }
    if (shape != Server::FirstFrameShape::Hello) {
        Refuse(*control, Protocol::RefuseCode::MalformedHello, Server::FirstFrameRefusalDetail(shape));
        ExitBeforeAccept(sourceFd, sourceKind, 0);
    }
    const auto* hello = Protocol::GetCtrlEnvelope(helloFrame.data())->msg_as_Hello();
    // PH-7 (1). The SAME policy ServerSession::Accept applies, from Handshake.h, so the
    // supervisor's pre-fork answer and the session's post-fork answer cannot drift. What went:
    // a `std::string` comparison (byte-at-a-time, early-returning) that ALSO refused a peer for
    // presenting a token to a server that had configured none, and a `tcp` guard that made the
    // unix control socket exempt by construction rather than by the policy's own reasoning.
    // RefuseHandshake logs `Refuse{Authentication}` and sends the peer the frame; the supervisor's
    // local Refuse() helper stays for the refusals that are its own (Busy, backend).
    //
    // PH-7 (5) (ph-f.md §6.4 (b)): the token is asked FIRST. ValidatePeerHandshake used to run
    // before it, so an unauthenticated peer with a wrong fingerprint was answered
    // Refuse{WireFingerprint} carrying this build's wire fingerprint - and Refuse{BuildFingerprint}
    // this build's stamp. An unauthenticated peer now learns only Refuse{Authentication}. On TCP
    // --serve the supervisor has already asked (the answer is the same; it is asked again so this
    // function holds the rule on its own for the unix and single-session shapes).
    if (AuthenticatePeerToken(*control, hello->token()) != MOBILEGL_OK) ExitBeforeAccept(sourceFd, sourceKind, 0);
    // Reject incompatible wire before backend bring-up can obscure the cause.
    if (ValidatePeerHandshake(*control, hello->abiMajor(), hello->abiMinor(),
            hello->wireFingerprint(), hello->buildFingerprint() ? hello->buildFingerprint()->c_str() : nullptr,
            hello->dialMode()) != MOBILEGL_OK) ExitBeforeAccept(sourceFd, sourceKind, 0);
    MobileGL::MG_ConfigLoader::Init();
    MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::Spawn;
    MobileGL::MG_Pipe::MGPipeSetServerProcessRole(true);
    // PH-1 (3), ID-P7-1: THIS PROCESS IS THE SESSION, so a named fault on the peer's bytes may end
    // it cleanly instead of aborting it (FatalFunnel.h's latch block). Armed here and nowhere
    // else: inproc keeps every Fatal (client and server share that process), and the supervisor
    // that forked this child never applies a record.
    ArmSessionLatch();
    // The child bypasses MobileGL::Initialize: arm its own role counters.
    MobileGL::MG_Util::PipeStats::Init();
    const auto backend = static_cast<MobileGL::BackendType>(hello->backendType());
    if (backend != MobileGL::BackendType::DirectGLES && backend != MobileGL::BackendType::DirectVulkan) {
        Refuse(*control, Protocol::RefuseCode::Backend, "unsupported Hello.backendType");
        ExitBeforeAccept(sourceFd, sourceKind, 0);
    }
    const char* pinned = std::getenv("MOBILEGL_BACKEND_TYPE");
    if (pinned && *pinned && backend != MobileGL::MG_Config::ActiveBackendType) {
        Refuse(*control, Protocol::RefuseCode::Backend, "Hello.backendType disagrees with pinned backend");
        ExitBeforeAccept(sourceFd, sourceKind, 0);
    }
    MobileGL::MG_Config::ActiveBackendType = backend;
    if (!MobileGL::MG_Backend::InitServerRoleForSpawn()) ExitBeforeAccept(sourceFd, sourceKind, 66);
    auto& session = Server::ServerSessionInstance();
    int serverBell[2] = {-1, -1}, clientBell[2] = {-1, -1};
    std::unique_ptr<SocketDoorbell> selfBell, peerBell;
    if (!tcp) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, serverBell) != 0 ||
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, clientBell) != 0) ExitBeforeAccept(sourceFd, sourceKind, 74);
        selfBell = std::make_unique<SocketDoorbell>(serverBell[0], serverBell[1], 1, true);
        peerBell = std::make_unique<SocketDoorbell>(-1, clientBell[1], 1, true);
        session.SetExternalDoorbells(selfBell.get(), peerBell.get());
    }
    if (tcp) session.SetDataConnectionSource(std::move(dataSource));
    const auto accepted = session.Accept(*control, &helloFrame);
    // THE SOURCE IS DONE WHEN ACCEPT IS, whichever way Accept went (F fix round). Under --serve
    // this is the child's end of the hand-off pair: closing it turns every later DataBind the
    // supervisor would have queued here into a send failure the supervisor refuses by name on
    // that connection ("data connection could not be handed to the live session"), instead of a
    // descriptor nobody will read holding a peer's connection open and, past max_dgram_qlen,
    // holding the supervisor itself. In the single-session shape it is the listener: kept open
    // only until the data connection arrived on it, and closed here so a second client or a
    // retry is refused at connect (ECONNREFUSED, as before PH-7 (4)) rather than completing a
    // TCP handshake this process would never accept and waiting out its whole connect budget.
    // The source object goes with it, so no later call can poll a descriptor number the kernel
    // may have reused. The hand-off is cut before it is closed, and what was already queued on
    // it is refused by name rather than dropped (ID-P7-44, CloseHandoffRefusingQueued).
    if (sourceFd >= 0) {
        if (sourceKind == DataSource::Handoff) CloseHandoffRefusingQueued(sourceFd);
        else ::close(sourceFd);
        session.SetDataConnectionSource({});
    }
    if (accepted != MOBILEGL_OK) {
        WireLogError("MG_Remote server: pid=%d Accept failed (rc=%d)", selfPid, static_cast<int>(accepted));
        std::fflush(nullptr);
        // A refusal and a peer that went away before its data connection bound (PH-7 (4): the
        // client closed after Welcome) are outcomes, not faults; the supervisor counts only 67.
        ::_exit(accepted == MOBILEGL_ERR_PROTOCOL_MISMATCH || accepted == MOBILEGL_ERR_TRANSPORT_CLOSED ? 0 : 67);
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
        // PH-6 fix round: THE SERVER'S BELL GETS A DEATH WITNESS TOO (CONTRACT-P6 D5c, mirrored).
        // selfBell parks on serverBell[0] and this process rings itself through serverBell[1], so
        // it holds both ends of that pair and the pair can never hang up, however dead the client
        // is: Dead() stayed false for the life of a shm session, and ReserveEventOrBlock's "dead
        // first" check could only ever report a peer killed mid-wait as NotDraining after the
        // whole MOBILEGL_IPC_EVENT_WAIT_MS. The control socket's far end is the client's alone, so
        // its hangup IS the death; the bell polls it for hangup only and never reads from it, so
        // SocketTransport's reassembler below keeps every byte. Set before loop.Start(): the apply
        // thread is the one that parks on this bell. (The stream plane's bell is the data socket
        // itself and dies with the peer already.)
        selfBell->SetDeathWitness(control->StreamFd());
    }

    const char* forwarding = std::getenv("MOBILEGL_IPC_LOG_FORWARD");
    if (tcp && (!forwarding || std::strcmp(forwarding, "0") != 0))
        MobileGL::MG_Util::Debug::SetLogForwarder(&ForwardLog, control.get());
    auto& loop = Server::ServerLoopInstance();
    if (loop.Start(session) != MOBILEGL_OK) ::_exit(70);
    WireLogError("MG_Remote server: pid=%d transport=spawn role=server ready control=%s data=%s",
                 selfPid, tcp ? "tcp" : "unix", tcp ? "stream" : "shm");
    std::vector<std::uint8_t> buffer(64 * 1024);
    // PH-6 (ID-P7-2): THE CONTROL WAIT IS BOUNDED, SO A SESSION CAN END FROM THE SERVER'S SIDE.
    // It used to be kWaitForever, which left exactly one way out: the peer closing its control
    // connection. A peer that forfeited the reverse channel is by definition one that stopped
    // reading, and it may well keep that connection open - so the apply thread would stop and
    // this child would sit here, holding the supervisor's one session slot, answering every
    // later client Busy. Now each quiet interval asks whether the apply thread is still there;
    // when it is not (forfeit, or a dead data bell), the session takes the ordinary exit below:
    // Stop, Close, _exit(0). (PH-6 used the supervisor's 250 ms accept poll as the interval; the
    // PH-1 note below says why the slice is now 100 ms.)
    //
    // ASKED AT THE TOP OF EVERY TURN, NOT ONLY WHEN A WAIT TIMES OUT (PH-6 fix round). A peer that
    // forfeited the reverse channel but keeps TALKING - a LogFlush or a surface op more often than
    // every 250 ms, each of which this loop answers - never let ReceiveFrame time out, so the
    // question was never asked and the session, with the supervisor's one slot, lived as long as
    // the peer kept chattering.
    //
    // PH-1 (3): the same wait is also how a LATCH raised on the apply thread (the decoder, the
    // applier, DrainRing) ends this loop within one slice while the peer is silent. ReceiveFrame
    // keeps a partial frame in its reassembler across a timeout, so slicing loses no bytes; a
    // latched session is then closed below with kSessionLatchedExitCode. The latch is asked
    // BEFORE the apply thread's liveness because a latch also stops that thread (it leaves its
    // loop and clears m_running), and the session must be named by its latch, not as a stopped
    // apply thread. One slice serves both: the latch's 100 ms (PH-6 had 250 ms; the shorter wait
    // only asks the question more often).
    constexpr std::uint32_t kControlSliceMs = 100;
    for (;;) {
        if (SessionLatched()) break;
        if (!loop.Running()) {
            WireLogError("MG_Remote server: pid=%d the apply thread has stopped (%s, %llu event(s) dropped); "
                         "ending the session",
                         selfPid, session.ReverseChannelForfeited() ? "ReverseChannelForfeit" : "its bell died",
                         static_cast<unsigned long long>(session.ForfeitDrops()));
            break;
        }
        std::uint64_t size = 0;
        const auto result = control->ReceiveFrame({buffer.data(), buffer.size()}, &size, kControlSliceMs);
        if (result == MOBILEGL_ERR_TIMEOUT) continue;
        if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL) { buffer.resize(static_cast<std::size_t>(size)); continue; }
        if (result != MOBILEGL_OK) {
            // EOF or a transport error: the peer is gone. Said to the session BEFORE loop.Stop()
            // below, so an apply thread that the stop wakes inside ReserveEventOrBlock names the
            // hangup (PeerGone) rather than the stop (PH-6 fix round).
            session.NoteControlStreamEnded();
            // Fuzz arm 1, steady state. A bad magic or an over-cap length has already been named
            // by the framing latch (Framing.h) as it latched; a clean close is the client's normal
            // end and says nothing. The one silent case was a close INSIDE a frame - a truncated
            // length prefix or payload - which ended the session exactly like a clean close.
            if (result == MOBILEGL_ERR_TRANSPORT_CLOSED && control->BufferedBytes() != 0)
                WireLogError("MG_Remote server: control connection closed inside a frame (%llu bytes of it "
                             "received); ending the session",
                             static_cast<unsigned long long>(control->BufferedBytes()));
            break;
        }
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
        if (!operation) {
            // Fuzz arm 1, steady state: a well-formed envelope of a type this loop does not take
            // (or a SurfaceOp tag with no table behind it) ended the session without a word.
            WireLogError("MG_Remote server: control frame of type %u (%s) is not a LogFlush or SurfaceOp "
                         "(size=%llu); ending the session",
                         static_cast<unsigned>(envelope->msg_type()), Protocol::EnumNameCtrlMsg(envelope->msg_type()),
                         static_cast<unsigned long long>(size));
            break;
        }
        Server::SurfaceControlFrame reply{};
        (void)ServerApplyWireSurfaceOp(*operation, &reply);
        // PH-1 (3): an op that latched (its own bytes, or a record the apply thread was on) is
        // answered by the SessionFault already sent and the close below, not by a reply.
        if (SessionLatched()) break;
        session.FlushDataProgress();
        if (session.DataLink()) reply.eventHead = session.DataLink()->EventPublishedHead();
        flatbuffers::FlatBufferBuilder builder(256);
        EncodeSurfaceReplyFrame(reply, &builder);
        if (control->SendFrame({builder.GetBufferPointer(), builder.GetSize()}) != MOBILEGL_OK) {
            session.NoteControlStreamEnded();
            break;
        }
    }
    loop.Stop();
    // Publish the final server window while log forwarding is still attached.
    MobileGL::MG_Util::PipeStats::Shutdown();
    // PH-1 (3): a latched session ends HERE, by the same orderly path as a peer's EOF - the apply
    // thread has already left its loop and destroyed the backend on its own thread (loop.Stop()
    // joined it), the session closes, and the exit status names the latch so the supervisor
    // counts it. The first fault's line was logged (and sent as a SessionFault) when it latched.
    const bool latched = SessionLatched();
    if (latched) {
        WireLogError("MG_Remote server: pid=%d session closed on a latched fault (%s; %llu named "
                     "fault(s) latched or declined); exiting %d for the supervisor to reap - PH-1",
                     selfPid, FatalFamilyName(SessionLatchedFamily()),
                     static_cast<unsigned long long>(SessionLatchCount()), kSessionLatchedExitCode);
    }
    session.Close();
    MobileGL::MG_Util::Debug::SetLogForwarder(nullptr, nullptr);
    // _exit closes control after cleanup/flush; peer EOF is the client's fence.
    std::fflush(nullptr);
    ::_exit(latched ? kSessionLatchedExitCode : 0);
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
        // PH-1 (3): a latched session is a named, orderly end - still a fault (counted), and said
        // as one so the reap line alone tells it from a crash or a refused bring-up.
        WireLogError("MG_Remote server: session pid=%d reaped exit=%d%s sessionsFaulted=%lu",
                     reaped, code, code == kSessionLatchedExitCode ? " (latched fault)" : "",
                     sessionsFaulted);
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

// PH-7 (5), ID-P7-3, ph-f.md §6.4, CONTRACT-P7 §12. THE TCP SUPERVISOR: AUTHENTICATE, THEN FORK.
//
// Until this package the --serve loop accepted a TCP connection and forked a session child for
// it before a byte had been read; the child read the Hello (10 s budget), checked the wire and
// only then the token. So anybody who could reach the port cost the server a fork per connection
// and a child held for up to 10 s, and an unauthenticated Hello with a wrong fingerprint was
// answered with this build's fingerprint. Now:
//
//   - every new connection is PENDING until its first frame is complete. The supervisor reads it
//     itself, a few bytes per wakeup, from all pending connections at once (one poll loop - no
//     unauthenticated peer can make it wait for another; the one wait left is up to 50 ms for an
//     exiting session before an AUTHENTICATED Hello is told Busy), and never a byte past the frame;
//   - a pending connection has MOBILEGL_IPC_PREAUTH_MS (default 2000) to complete it, and at most
//     MOBILEGL_IPC_PREAUTH_MAX (default 8) are pending at once. The queue is SHARED between
//     addresses: when it is full a newcomer displaces the oldest connection of the address holding
//     the most slots, and is itself refused Busy only when its own address already holds as many;
//   - a Hello is AUTHENTICATED HERE, token first; an unauthenticated one is answered
//     Refuse{Authentication} and nothing else. Only an authenticated Hello is forked for, and the
//     child is handed the frame it would otherwise have read;
//   - a failure (wrong token, malformed first frame, deadline, a silent connection that held its
//     slot past kPreAuthProbeGraceMs before it closed or was displaced) is counted against the
//     peer's address; after MOBILEGL_IPC_AUTH_BACKOFF_AFTER (default 5) of them that address is refused
//     at accept, before a byte is read, for MOBILEGL_IPC_AUTH_BACKOFF_MS doubling per further
//     failure up to a minute. An authenticated Hello from the address clears it;
//   - a DataBind is routed exactly as PH-7 (4) routed it: to the live session's child over the
//     hand-off with the nonce as sideband (the child compares), or refused by name when there is
//     no live session - here now, instead of in a child forked to say so.
//
// What is unchanged: one session at a time (an AUTHENTICATED second client is Busy), the child's
// own ValidatePeerHandshake / Accept / BindDataConnection, the hand-off and its refusals, the
// reap lines and sessionsFaulted.
class TcpSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    TcpSupervisor(int listener, const Server::PreAuthKnobs& knobs, unsigned long& sessionsFaulted)
        : m_listener(listener), m_knobs(knobs), m_backoff(knobs), m_sessionsFaulted(sessionsFaulted) {}

    // Returns only on a failure of the listener itself (73, as the old loop did).
    int Run() {
        WireLogError("MG_Remote server: pre-auth deadline=%u ms, at most %u pending, backoff after %u failures "
                     "from %u ms (0 = off)",
                     m_knobs.deadlineMs, m_knobs.maxPending, m_knobs.backoffAfter, m_knobs.backoffBaseMs);
        std::vector<pollfd> fds;
        for (;;) {
            Reap();
            auto now = Clock::now();
            // While accepting is paused (out of descriptors, AcceptNew) the listener sits out the
            // poll - it stays readable, and would spin the loop - and the pause's end is a wakeup.
            const bool acceptPaused = now < m_acceptResumeAt;
            fds.clear();
            fds.push_back({acceptPaused ? -1 : m_listener, POLLIN, 0});
            for (const auto& peer : m_pending) fds.push_back({peer.fd, POLLIN, 0});
            // 250 ms is the old accept timeout, i.e. how often a finished child is reaped while
            // nothing arrives; a pending deadline that falls sooner shortens it.
            std::int64_t timeout = 250;
            for (const auto& peer : m_pending)
                timeout = std::clamp<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(peer.deadline - now).count(), 0, timeout);
            if (acceptPaused)
                timeout = std::clamp<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(m_acceptResumeAt - now).count() + 1, 0,
                    timeout);
            const int polled = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(timeout));
            if (polled < 0 && errno != EINTR) {
                WireLogError("MG_Remote server: supervisor poll failed: %s", std::strerror(errno));
                return 73;
            }
            now = Clock::now();
            std::vector<PendingPeer> waiting;
            for (std::size_t index = 0; index < m_pending.size(); ++index) {
                PendingPeer& peer = m_pending[index];
                if (polled > 0 && fds[index + 1].revents != 0) {
                    switch (peer.frame.Pump(peer.fd)) {
                        case Step::NeedMore:
                            break;
                        case Step::Complete:
                            m_ready.push_back(std::move(peer));
                            continue;
                        case Step::NotAFrame:
                            // Wrong magic, or a length over 1 MiB: refused on the HEADER, before a
                            // byte of payload is allocated for.
                            RefuseAndClose(peer.fd, Protocol::RefuseCode::MalformedHello, Server::kFirstFrameNotAFrame);
                            NoteFailure(peer.address, "not a control frame");
                            continue;
                        case Step::EndedEmpty:
                            // A port probe - gone within the grace, having sent nothing - cost an
                            // accept and nothing else, and is not a failure. A connection that HELD
                            // its slot past the grace and then left without a byte is how one
                            // address kept every slot taken without ever reaching the deadline,
                            // so that one counts (fix round; PreAuthGate.h).
                            ::close(peer.fd);
                            if (now - peer.accepted >= std::chrono::milliseconds(Server::kPreAuthProbeGraceMs))
                                NoteFailure(peer.address, "held a pre-auth slot and closed without a byte");
                            continue;
                        case Step::EndedPartial:
                            RefuseAndClose(peer.fd, Protocol::RefuseCode::MalformedHello, Server::kFirstFrameTruncated);
                            NoteFailure(peer.address, "truncated first frame");
                            continue;
                    }
                }
                if (now >= peer.deadline) {
                    RefuseAndClose(peer.fd, Protocol::RefuseCode::Authentication, Server::kPreAuthDeadline);
                    NoteFailure(peer.address, "pre-auth deadline");
                    continue;
                }
                waiting.push_back(std::move(peer));
            }
            m_pending.swap(waiting);
            for (auto& peer : m_ready) Judge(peer);
            m_ready.clear();
            if (polled > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
                const int failed = AcceptNew(now);
                if (failed != 0) return failed;
            }
        }
    }

private:
    using Step = Server::FirstFrameAssembler::Step;

    struct PendingPeer {
        int fd = -1;
        std::string address;
        Clock::time_point accepted{};
        Clock::time_point deadline{};
        Server::FirstFrameAssembler frame;
    };

    bool Reap() {
        const bool reaped = ReapActiveSession(m_active, m_sessionsFaulted);
        if (m_active <= 0 && m_handoff >= 0) { ::close(m_handoff); m_handoff = -1; }
        return reaped;
    }

    // exit_group closes files just before waitpid can observe the exit. Allow that small
    // scheduling window before deciding whether a session is live - the old loop gave it to every
    // connection it accepted while a child existed; this gives it to the one decision that
    // depends on the answer: an AUTHENTICATED Hello's Busy (a client reconnecting the moment its
    // last session ended must not be told Busy by a zombie). Only a peer holding the token can
    // make the loop wait here; a DataBind's routing takes one look (Judge, fix round).
    void WaitOutAnExitingSession() {
        const auto reapDeadline = Clock::now() + std::chrono::milliseconds(50);
        while (m_active > 0 && Clock::now() < reapDeadline) {
            if (Reap()) break;
            ::usleep(1000);
        }
    }

    void NoteFailure(const std::string& address, const char* why) {
        const std::uint32_t window = m_backoff.NoteFailure(address, Clock::now());
        if (window != 0)
            WireLogError("MG_Remote server: %s failed pre-auth %u times (last: %s); refusing it at accept for %u ms",
                         address.c_str(), m_backoff.Failures(address), why, window);
    }

    // Accepts everything the listener has, deciding at accept what can be decided without reading.
    int AcceptNew(Clock::time_point now) {
        for (int burst = 0; burst < 64; ++burst) {
            int fd = -1;
            const auto accepted = SocketTransport::AcceptOne(m_listener, 0, &fd);
            if (accepted == MOBILEGL_ERR_TIMEOUT) return 0;
            if (accepted == MOBILEGL_ERR_OUT_OF_MEMORY) {
                // f2-auth fix round. Out of descriptors (or memory) is not a dead listener: the
                // connection waits in the backlog, and pending connections give descriptors back
                // as they are judged or reach their deadline. The first cut returned 73 here, so
                // a connection flood against a low RLIMIT_NOFILE (or a large pending cap) took the
                // supervisor down. Now accepting pauses for kAcceptPauseMs and the loop goes on.
                m_acceptResumeAt = now + std::chrono::milliseconds(kAcceptPauseMs);
                WireLogError("MG_Remote server: accept paused for %u ms: out of descriptors or memory with %zu "
                             "connections awaiting authentication",
                             kAcceptPauseMs, m_pending.size());
                return 0;
            }
            if (accepted != MOBILEGL_OK) return 73;
            std::string address = PeerAddress(fd);
            if (const std::uint32_t retry = m_backoff.RetryAfterMs(address, now); retry != 0) {
                WireLogError("MG_Remote server: connection from %s refused at accept: authentication backoff, "
                             "%u ms left", address.c_str(), retry);
                RefuseAndClose(fd, Protocol::RefuseCode::Authentication, Server::kAuthBackoffRefusal);
                continue;
            }
            if (m_pending.size() >= m_knobs.maxPending) {
                // Full. The queue is shared between addresses (PreAuthGate.h
                // ChoosePendingToDisplace): the newcomer takes the busiest address's oldest slot,
                // unless its own address is the busiest - then it is the one refused.
                std::vector<std::string> holders;
                holders.reserve(m_pending.size());
                for (const auto& peer : m_pending) holders.push_back(peer.address);
                const std::size_t victim = Server::ChoosePendingToDisplace(holders, address);
                if (victim >= m_pending.size()) {
                    WireLogError("MG_Remote server: connection from %s refused at accept: %zu connections are "
                                 "already awaiting authentication", address.c_str(), m_pending.size());
                    RefuseAndClose(fd, Protocol::RefuseCode::Busy, Server::kPreAuthFull);
                    continue;
                }
                PendingPeer displaced = std::move(m_pending[victim]);
                m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(victim));
                WireLogError("MG_Remote server: pending connection from %s displaced by one from %s: the pre-auth "
                             "queue is full and %s holds the most of it", displaced.address.c_str(), address.c_str(),
                             displaced.address.c_str());
                RefuseAndClose(displaced.fd, Protocol::RefuseCode::Busy, Server::kPreAuthDisplaced);
                // A displaced connection that had already held its slot past the grace is a silent
                // holder like one that closed; otherwise displacement would be a free way out.
                if (now - displaced.accepted >= std::chrono::milliseconds(Server::kPreAuthProbeGraceMs))
                    NoteFailure(displaced.address, "held a pre-auth slot until displaced");
            }
            PendingPeer peer;
            peer.fd = fd;
            peer.address = std::move(address);
            peer.accepted = now;
            peer.deadline = now + std::chrono::milliseconds(m_knobs.deadlineMs);
            m_pending.push_back(std::move(peer));
        }
        return 0;
    }

    // A complete first frame. Every refusal here is by name; only an authenticated Hello forks.
    void Judge(PendingPeer& peer) {
        auto connection = std::make_unique<SocketTransport>(peer.fd, -1, TransportRole::Server);
        const int fd = peer.fd;
        peer.fd = -1; // `connection` owns it now
        const auto& payload = peer.frame.Payload();
        const auto shape = Server::ClassifyFirstFrame(payload.data(), payload.size());
        if (shape == Server::FirstFrameShape::DataBind) {
            // PH-7 (4). While a session is live, its data connection goes to its child with the
            // nonce it presented (the child compares, once); with none live there is nobody it
            // could belong to. Neither is an authentication failure: a data connection left over
            // from a session that just ended is a legitimate client's.
            std::uint8_t nonce[kDataNonceBytes] = {};
            (void)DecodeDataBind(payload, nonce);
            // One look, not WaitOutAnExitingSession's 50 ms (fix round): DataBinds are
            // unauthenticated and never counted, so waiting for each would let any peer stall this
            // loop 50 ms per connection. Nothing needs the wait here - a child that is exiting
            // either has cut its hand-off already (the send below fails and is refused by name) or
            // refuses what is queued on its way out (ExitBeforeAccept / CloseHandoffRefusingQueued).
            Reap();
            if (m_active <= 0) {
                // StreamLink's frames may already follow the DataBind; read them away so the close
                // does not become an RST that discards the refusal.
                DrainUnread(fd);
                Refuse(*connection, Protocol::RefuseCode::Authentication, "data connection names no live session");
                return;
            }
            // The send is MSG_DONTWAIT and the child cuts its end the moment its own data
            // connection is bound (CloseHandoffRefusingQueued), so this either queues the
            // descriptor for a child that will read or refuse it, or fails and is refused here.
            if (m_handoff >= 0 && FdPassing::SendFd(m_handoff, fd, MobileGLByteSpan{nonce, sizeof(nonce)},
                                                    /*dontWait=*/true) == MOBILEGL_OK) {
                // The child holds its own descriptor now; ours goes without a shutdown(2), which
                // would disconnect the child's too.
                connection->CloseLocalCopy();
                return;
            }
            DrainUnread(fd);
            Refuse(*connection, Protocol::RefuseCode::Authentication, kHandoffRefused);
            return;
        }
        if (shape != Server::FirstFrameShape::Hello) {
            DrainUnread(fd);
            Refuse(*connection, Protocol::RefuseCode::MalformedHello, Server::FirstFrameRefusalDetail(shape));
            NoteFailure(peer.address, "malformed first frame");
            return;
        }
        const auto* hello = Protocol::GetCtrlEnvelope(payload.data())->msg_as_Hello();
        // The token FIRST, and the only answer an unauthenticated peer gets is this one
        // (RefuseHandshake: Refuse{Authentication} "token mismatch", no fingerprint, no stamp).
        if (AuthenticatePeerToken(*connection, hello->token()) != MOBILEGL_OK) {
            // Anything the peer sent after its Hello is read away before the close, so the close
            // cannot become an RST that discards the refusal already in flight.
            DrainUnread(fd);
            WireLogError("MG_Remote server: unauthenticated Hello from %s refused before any session work",
                         peer.address.c_str());
            NoteFailure(peer.address, "token mismatch");
            return;
        }
        m_backoff.NoteSuccess(peer.address);
        WaitOutAnExitingSession();
        if (m_active > 0) {
            // The frame is consumed, so closing cannot RST the refusal away.
            Refuse(*connection, Protocol::RefuseCode::Busy, "one session is already active");
            return;
        }
        Fork(std::move(connection), payload);
    }

    void Fork(std::unique_ptr<SocketTransport> connection, const std::vector<std::uint8_t>& hello) {
        int pair[2] = {-1, -1};
        if (FdPassing::CreateSocketPair(pair) != MOBILEGL_OK) {
            Refuse(*connection, Protocol::RefuseCode::Busy, "could not create the data-connection hand-off");
            return;
        }
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) {
            // The child owns this one connection and its end of the hand-off. Every OTHER pending
            // peer's descriptor is closed here, or the supervisor's refusal of that peer would not
            // end its connection while this session lives.
            ::close(m_listener);
            ::close(pair[0]);
            for (const auto& other : m_pending)
                if (other.fd >= 0) ::close(other.fd);
            for (const auto& other : m_ready)
                if (other.fd >= 0) ::close(other.fd);
            const int controlFd = connection->StreamFd();
            RunSession(std::move(connection), hello, HandoffSource(pair[1], controlFd), pair[1], DataSource::Handoff,
                       0);
        }
        if (child < 0) {
            ::close(pair[0]);
            ::close(pair[1]);
            Refuse(*connection, Protocol::RefuseCode::Busy, "could not create session child");
            return;
        }
        m_active = child;
        ::close(pair[1]);
        m_handoff = pair[0];
        connection->CloseLocalCopy();
    }

    int m_listener;
    Server::PreAuthKnobs m_knobs;
    Server::AuthBackoff m_backoff;
    unsigned long& m_sessionsFaulted;
    pid_t m_active = -1;
    // The supervisor's end of the live child's data-connection hand-off (PH-7 (4)); -1 while no
    // session is live. It goes with the child: a reaped session can be handed nothing.
    int m_handoff = -1;
    std::vector<PendingPeer> m_pending;
    std::vector<PendingPeer> m_ready;
    // Accepting resumes at this time after accept(2) ran out of descriptors or memory.
    static constexpr std::uint32_t kAcceptPauseMs = 100;
    Clock::time_point m_acceptResumeAt{};
};
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
    unsigned long sessionsFaulted = 0;
    const bool tcpEndpoint = endpoint.compare(0, 6, "tcp://") == 0;
    const auto knobs = Server::PreAuthKnobs::FromEnvironment();
    if (tcpEndpoint && serve) {
        // PH-7 (5): the TCP supervisor reads and authenticates every first frame before it forks.
        TcpSupervisor supervisor(listener, knobs, sessionsFaulted);
        const int failed = supervisor.Run();
        LogSupervisorSummary(sessionsFaulted);
        ::close(listener);
        return failed;
    }
    // From here: the unix endpoint (--serve or not), and the single-session TCP shape.
    pid_t active = -1;
    const auto reap = [&]() { return ReapActiveSession(active, sessionsFaulted); };
    for (;;) {
        reap();
        std::unique_ptr<SocketTransport> control;
        MobileGLResult accepted = MOBILEGL_ERR_UNSUPPORTED;
        if (tcpEndpoint) {
            int fd = -1;
            accepted = SocketTransport::AcceptOne(listener, 30000, &fd);
            if (accepted == MOBILEGL_OK) control = std::make_unique<SocketTransport>(fd, -1, TransportRole::Server);
        } else {
            accepted = SocketTransport::AcceptPair(listener, serve ? 250 : 30000, control);
        }
        if (accepted == MOBILEGL_ERR_TIMEOUT && serve) continue;
        if (accepted != MOBILEGL_OK) { LogSupervisorSummary(sessionsFaulted); ::close(listener); return 73; }
        if (!serve) {
            if (tcpEndpoint) {
                // The listener stays open until this session's data connection has arrived on
                // it; RunSession closes it the moment Accept returns. The process IS the session,
                // so there is no fork to spare; the Hello is still read within the pre-auth
                // deadline and the token is still asked first.
                const int controlFd = control->StreamFd();
                RunSession(std::move(control), {}, ListenerSource(listener, controlFd), listener,
                           DataSource::Listener, knobs.deadlineMs);
            }
            ::close(listener);
            if (endpoint[0] != '@') ::unlink(endpoint.c_str());
            RunSession(std::move(control), {}, {}, -1, DataSource::None, 10000);
        }
        reap();
        // exit_group closes files just before waitpid can observe the exit.
        // Allow that small scheduling window; a live peer remains Busy.
        const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (active > 0 && std::chrono::steady_clock::now() < reapDeadline) {
            if (reap()) break;
            ::usleep(1000);
        }
        if (active > 0) {
            RefuseBusy(*control, "one session is already active");
            continue;
        }
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(listener);
            RunSession(std::move(control), {}, {}, -1, DataSource::None, 10000);
        }
        if (child < 0) {
            RefuseBusy(*control, "could not create session child");
            continue;
        }
        active = child;
        control->CloseLocalCopy();
    }
    LogSupervisorSummary(sessionsFaulted);
    ::close(listener);
    if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
    return 0;
}
