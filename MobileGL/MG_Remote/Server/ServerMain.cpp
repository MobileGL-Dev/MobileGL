// MobileGL - separate-process server and TCP session supervisor.
// SPDX-License-Identifier: LGPL-3.0-only
#include "../Transport/Doorbell.h"
#include "../Transport/FdPassing.h"
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
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <cerrno>
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

// PH-7 (4), ID-P7-3. THE DATA CONNECTION IS ROUTED BY WHAT IT SAYS, NOT BY WHEN IT CAME.
//
// On TCP the supervisor accepts ONE connection at a time. While no session is live, a
// connection is a session's control connection and goes to a fresh child. While one is, the
// connection's first frame decides: a DataBind is the live session's data connection and its
// descriptor goes to that child over the hand-off socketpair, with the nonce it presented as
// the sideband; anything else is a second client and is refused Busy exactly as before. The
// supervisor never judges the nonce - ServerSession::BindDataConnection compares it, once, in
// the child that minted it. This replaces AcceptPair's rule that the second connection to
// arrive inside the window IS the data plane: over a Windows `adb forward` the two arrived
// reordered, the data connection was read as control, and every child exited 67.
//
// THE HAND-OFF HAS ONE READER, AND IT STOPS READING (F fix round, review of slices 1-3). The
// child reads the hand-off only inside BindDataConnection; once its data connection is bound
// it never reads it again. The first shape of this function kept forwarding every later
// DataBind regardless: each descriptor sat in the child's receive queue with no reply, its TCP
// connection was held open until the session ended, and once the queue reached
// net.unix.max_dgram_qlen (10 on the phone's kernel, 512 on a desktop's) the supervisor BLOCKED
// in sendmsg for the rest of the session - no accepts, no Busy refusals, no reaping. Two halves
// close it: the child closes its end of the pair the moment Accept returns (RunSession), so the
// send fails with "peer gone" and the refusal below fires by name; and the send is MSG_DONTWAIT,
// so a queue that is full for any other reason refuses too rather than stalling the one process
// that answers the port.
constexpr std::uint32_t kFirstFrameWaitMs = 2000;
constexpr std::uint64_t kFirstFrameMaxBytes = 1024 * 1024;

// Whatever the peer has already sent, read and dropped (non-blocking, at most 1 MiB), so a close
// with bytes still unread does not become an RST that discards the refusal in flight - the
// reason RefuseBusy reads the Hello before refusing.
void DrainUnread(int fd) {
    char sink[4096];
    for (int rounds = 0; rounds < 256; ++rounds) {
        if (::recv(fd, sink, sizeof(sink), MSG_DONTWAIT) <= 0) return;
    }
}

void RouteWhileBusy(SocketTransport& connection, int handoff) {
    std::vector<std::uint8_t> frame;
    const int fd = connection.StreamFd();
    const auto read = SocketTransport::ReceiveOneFrame(fd, kFirstFrameWaitMs, kFirstFrameMaxBytes, &frame);
    if (read == MOBILEGL_ERR_PROTOCOL_MISMATCH) {
        // (F fix round) A first frame with the wrong magic or a length over 1 MiB is not a second
        // client waiting its turn; it is not a control frame at all, and Busy would name a
        // condition this connection does not have. The word is the one ServerSession::Accept uses
        // for a first frame that is not a verifiable Hello.
        DrainUnread(fd);
        Refuse(connection, Protocol::RefuseCode::MalformedHello, "first frame is not a control frame");
        return;
    }
    std::uint8_t nonce[kDataNonceBytes] = {};
    if (read == MOBILEGL_OK && DecodeDataBind(frame, nonce)) {
        if (handoff >= 0 &&
            FdPassing::SendFd(handoff, fd, MobileGLByteSpan{nonce, sizeof(nonce)}, /*dontWait=*/true) == MOBILEGL_OK) {
            // The child holds its own descriptor for this connection now; ours goes without a
            // shutdown(2), which would disconnect the child's too.
            connection.CloseLocalCopy();
            return;
        }
        Refuse(connection, Protocol::RefuseCode::Authentication,
               "data connection could not be handed to the live session");
        return;
    }
    // The frame (if any) is consumed, so closing cannot RST the refusal away - the reason
    // RefuseBusy reads the Hello first.
    Refuse(connection, Protocol::RefuseCode::Busy, "one session is already active");
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

// Called only after fork: the supervisor never creates a backend or EGL context.
// `dataSource` is set on TCP (PH-7 (4)) and empty on a unix endpoint, whose second connection
// is the SCM_RIGHTS socket AcceptPair already paired. `sourceFd` is the descriptor that source
// reads from - the child's end of the hand-off socketpair under --serve, the listener itself in
// the single-session shape - and -1 when there is none; it is closed the moment Accept returns.
[[noreturn]] void RunSession(std::unique_ptr<SocketTransport> control,
                             Server::ServerSession::DataConnectionSource dataSource, int sourceFd) {
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
    // PH-7 (4). A DataBind with no live session to join: the supervisor routes a data connection
    // to a child only while one is live, so this is a stale connection (its session already
    // ended) or one that never had a session. Refused by name, and the child is a clean exit -
    // it is a refusal, not a fault.
    if (Protocol::GetCtrlEnvelope(helloFrame.data())->msg_type() == Protocol::CtrlMsg::DataBind) {
        Refuse(*control, Protocol::RefuseCode::Authentication, "data connection names no live session");
        std::fflush(nullptr);
        ::_exit(0);
    }
    const auto* hello = Protocol::GetCtrlEnvelope(helloFrame.data())->msg_as_Hello();
    if (!hello) ::_exit(67);
    // Reject incompatible wire before backend bring-up can obscure the cause.
    if (ValidatePeerHandshake(*control, hello->abiMajor(), hello->abiMinor(),
            hello->wireFingerprint(), hello->buildFingerprint() ? hello->buildFingerprint()->c_str() : nullptr,
            hello->dialMode()) != MOBILEGL_OK) ::_exit(0);
    // PH-7 (1). The SAME policy ServerSession::Accept applies, from Handshake.h, so the
    // supervisor's pre-fork answer and the session's post-fork answer cannot drift. What went:
    // a `std::string` comparison (byte-at-a-time, early-returning) that ALSO refused a peer for
    // presenting a token to a server that had configured none, and a `tcp` guard that made the
    // unix control socket exempt by construction rather than by the policy's own reasoning.
    // RefuseHandshake logs `Refuse{Authentication}` and sends the peer the frame; the supervisor's
    // local Refuse() helper stays for the refusals that are its own (Busy, backend).
    if (AuthenticatePeerToken(*control, hello->token()) != MOBILEGL_OK) ::_exit(0);
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
    // may have reused.
    if (sourceFd >= 0) {
        ::close(sourceFd);
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
    // Stop, Close, _exit(0). The interval is the supervisor's own accept poll.
    //
    // ASKED AT THE TOP OF EVERY TURN, NOT ONLY WHEN A WAIT TIMES OUT (PH-6 fix round). A peer that
    // forfeited the reverse channel but keeps TALKING - a LogFlush or a surface op more often than
    // every 250 ms, each of which this loop answers - never let ReceiveFrame time out, so the
    // question was never asked and the session, with the supervisor's one slot, lived as long as
    // the peer kept chattering.
    constexpr std::uint32_t kControlPollMs = 250;
    for (;;) {
        if (!loop.Running()) {
            WireLogError("MG_Remote server: pid=%d the apply thread has stopped (%s, %llu event(s) dropped); "
                         "ending the session",
                         selfPid, session.ReverseChannelForfeited() ? "ReverseChannelForfeit" : "its bell died",
                         static_cast<unsigned long long>(session.ForfeitDrops()));
            break;
        }
        std::uint64_t size = 0;
        const auto result = control->ReceiveFrame({buffer.data(), buffer.size()}, &size, kControlPollMs);
        if (result == MOBILEGL_ERR_TIMEOUT) continue;
        if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL) { buffer.resize(static_cast<std::size_t>(size)); continue; }
        if (result != MOBILEGL_OK) {
            // EOF or a transport error: the peer is gone. Said to the session BEFORE loop.Stop()
            // below, so an apply thread that the stop wakes inside ReserveEventOrBlock names the
            // hangup (PeerGone) rather than the stop (PH-6 fix round).
            session.NoteControlStreamEnded();
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
        if (!operation) break;
        Server::SurfaceControlFrame reply{};
        (void)ServerApplyWireSurfaceOp(*operation, &reply);
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
    const bool tcpEndpoint = endpoint.compare(0, 6, "tcp://") == 0;
    // The supervisor's end of the live child's data-connection hand-off (PH-7 (4)); -1 while no
    // session is live. It goes with the child: a reaped session can be handed nothing.
    int handoff = -1;
    const auto reap = [&]() {
        const bool reaped = ReapActiveSession(active, sessionsFaulted);
        if (active <= 0 && handoff >= 0) { ::close(handoff); handoff = -1; }
        return reaped;
    };
    for (;;) {
        reap();
        std::unique_ptr<SocketTransport> control;
        MobileGLResult accepted = MOBILEGL_ERR_UNSUPPORTED;
        if (tcpEndpoint) {
            int fd = -1;
            accepted = SocketTransport::AcceptOne(listener, serve ? 250 : 30000, &fd);
            if (accepted == MOBILEGL_OK) control = std::make_unique<SocketTransport>(fd, -1, TransportRole::Server);
        } else {
            accepted = SocketTransport::AcceptPair(listener, serve ? 250 : 30000, control);
        }
        if (accepted == MOBILEGL_ERR_TIMEOUT && serve) continue;
        if (accepted != MOBILEGL_OK) { LogSupervisorSummary(sessionsFaulted); ::close(listener); return 73; }
        if (!serve) {
            if (tcpEndpoint) {
                // The listener stays open until this session's data connection has arrived on
                // it; RunSession closes it the moment Accept returns.
                const int controlFd = control->StreamFd();
                RunSession(std::move(control), ListenerSource(listener, controlFd), listener);
            }
            ::close(listener);
            if (endpoint[0] != '@') ::unlink(endpoint.c_str());
            RunSession(std::move(control), {}, -1);
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
            if (tcpEndpoint) RouteWhileBusy(*control, handoff);
            else RefuseBusy(*control, "one session is already active");
            continue;
        }
        int pair[2] = {-1, -1};
        if (tcpEndpoint && FdPassing::CreateSocketPair(pair) != MOBILEGL_OK) {
            RefuseBusy(*control, "could not create the data-connection hand-off");
            continue;
        }
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(listener);
            if (pair[0] >= 0) ::close(pair[0]);
            const int controlFd = control->StreamFd();
            RunSession(std::move(control), tcpEndpoint ? HandoffSource(pair[1], controlFd)
                                                       : Server::ServerSession::DataConnectionSource{},
                       pair[1]);
        }
        if (child < 0) {
            if (pair[0] >= 0) ::close(pair[0]);
            if (pair[1] >= 0) ::close(pair[1]);
            RefuseBusy(*control, "could not create session child");
            continue;
        }
        active = child;
        if (pair[1] >= 0) ::close(pair[1]);
        handoff = pair[0];
        control->CloseLocalCopy();
    }
    LogSupervisorSummary(sessionsFaulted);
    ::close(listener);
    if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
    return 0;
}
