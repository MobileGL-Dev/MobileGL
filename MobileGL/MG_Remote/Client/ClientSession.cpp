// MobileGL - MobileGL/MG_Remote/Client/ClientSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5: construction, the handshake and lifetime are package s1's; the verb barrier and the
// reply read that sits inside it are package c1's (EmitAndWait below is still c0's stub).

#include "ClientSession.h"

#include "../CapsCodec.h"
#include "../Protocol/generated/protocol_generated.h"
#include "../Server/ServerLoop.h"
#include "../Server/ServerSession.h"
#include "../Transport/InProcessTransport.h"

#include <MGGitHash.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#include <vector>

namespace MobileGL::MG_Remote::Client {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedClientSession, \"%s\"} - P5 packages s1/c1 have not "                      \
                "landed this yet; c0 shipped the signature only",                                                      \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    namespace {

        ClientSession* g_active = nullptr;

        // The same bounded handshake deadline the server uses. Bounded, not kWaitForever: a
        // bring-up that never answers has to be a red lane rather than a wedged CI job.
        constexpr Uint32 kHandshakeTimeoutMs = 5000;
        // Teardown's drain. Also bounded, and for the same reason - table 3's order is
        // "publish and wait for the server to drain and acknowledge", and a wait with no
        // deadline there turns a lost record into a hung process exit.
        constexpr Uint32 kDrainTimeoutMs = 5000;

        MobileGLResult ReceiveEnvelope(Transport::ITransport& transport, std::vector<Uint8>& out,
                                       Uint32 timeoutMs) {
            Uint64 size = 0;
            MobileGLMutableByteSpan empty{nullptr, 0};
            const MobileGLResult probe = transport.ReceiveFrame(empty, &size, timeoutMs);
            if (probe != MOBILEGL_ERR_BUFFER_TOO_SMALL) {
                return probe == MOBILEGL_OK ? MOBILEGL_ERR_PROTOCOL_MISMATCH : probe;
            }
            out.resize(static_cast<SizeT>(size));
            MobileGLMutableByteSpan span{out.data(), out.size()};
            return transport.ReceiveFrame(span, &size, 0);
        }

        const ::MobileGL::Wire::CtrlEnvelope* ParseEnvelope(const std::vector<Uint8>& bytes) {
            ::flatbuffers::Verifier verifier(bytes.data(), bytes.size());
            if (!::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier)) {
                return nullptr;
            }
            if (!::MobileGL::Wire::CtrlEnvelopeBufferHasIdentifier(bytes.data())) {
                return nullptr;
            }
            return ::MobileGL::Wire::GetCtrlEnvelope(bytes.data());
        }

        [[noreturn]] void FatalAbiMismatch(const char* what, Uint64 ours, Uint64 theirs,
                                           const char* theirStamp) {
            MGLOG_F("MGPipe: Fatal{AbiMismatch, \"%s\"} ours=%llu theirs=%llu ourBuild=%s "
                    "theirBuild=%s - never a downgrade: the caps block's size is ABI-dependent "
                    "and every field past the first difference would be read at the wrong offset",
                    what, static_cast<unsigned long long>(ours),
                    static_cast<unsigned long long>(theirs), GIT_COMMIT_HASH_SHORT,
                    theirStamp == nullptr ? "?" : theirStamp);
            std::abort();
        }

        const char* TransportModeName(MG_Config::TransportMode mode) {
            switch (mode) {
                case MG_Config::TransportMode::Monolith: return "monolith";
                case MG_Config::TransportMode::InProcess: return "inproc";
                case MG_Config::TransportMode::Spawn: return "spawn";
                case MG_Config::TransportMode::UnixSocket: return "unix:";
                case MG_Config::TransportMode::NamedPipe: return "pipe:";
            }
            return "?";
        }

    } // namespace

    // Null, not a Fatal: MG_Backend::Init() asks whether a session exists before it decides to
    // install the remote backend object, and that question has a legitimate "no" - it is the
    // monolith answer. Every call that PRESUMES a session aborts instead.
    ClientSession* ClientSession::Active() { return g_active; }

    ClientSession& ClientSessionInstance() {
        // Leak at exit, deliberately and per ID-8, exactly as ServerSessionInstance does.
        static ClientSession* instance = new ClientSession{};
        return *instance;
    }

    ClientSession::~ClientSession() {
        if (m_started) {
            Stop();
        }
    }

    Bool ClientSession::Started() const { return m_started; }

    MobileGLResult ClientSession::Start(MG_Config::TransportMode mode, const String& endpoint) {
        if (m_started) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // A NAMED ERROR, NEVER A FALLBACK TO MONOLITH. A silent fallback here is exactly the
        // "the split lane ran monolith and went green" failure the whole phase is built to
        // make impossible (ARCHITECTURE.md 10.3), so every mode this build cannot serve is
        // refused by name rather than degraded.
        if (mode != MG_Config::TransportMode::InProcess) {
            MGLOG_E("MG_Remote client: MOBILEGL_TRANSPORT=%s%s is refused by name - P5 implements "
                    "`inproc` only, and falling back to monolith would make this lane green for "
                    "the wrong reason. spawn / unix: / pipe: are P6's",
                    TransportModeName(mode), endpoint.empty() ? "" : endpoint.c_str());
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        // ---- 1. the control plane and the two bells. The transport owns the bells; THE
        // SESSION owns the rings, and the accessors stay off ITransport (contract §3.9).
        Transport::InProcessTransport::CreatePair(m_clientTransport, m_serverTransport);
        m_transport = m_clientTransport.get();

        // ---- 2. Hello. Sent before the server accepts: InProcessTransport queues whole
        // messages, so one thread can drive both halves of the handshake in order.
        const Uint64 fingerprint = CapsAbiFingerprint();
        {
            ::flatbuffers::FlatBufferBuilder builder(512);
            auto stamp = builder.CreateString(GIT_COMMIT_HASH_SHORT);
            auto hello = ::MobileGL::Wire::CreateHello(
                builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR, stamp,
                /*backendType=*/0u, /*pid=*/0u, /*configBlob=*/0, fingerprint);
            auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
                builder, ::MobileGL::Wire::CtrlMsg::Hello, hello.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
            const MobileGLResult sent = m_transport->SendFrame(
                MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()});
            if (sent != MOBILEGL_OK) {
                Stop();
                return sent;
            }
        }

        // ---- 3. the server half: ABI assert, four segments, Welcome.
        Server::ServerSession& server = Server::ServerSessionInstance();
        const MobileGLResult accepted = server.Accept(*m_serverTransport);
        if (accepted != MOBILEGL_OK) {
            Stop();
            return accepted;
        }

        // ---- 4. Welcome, and this side's half of the ABI assertion.
        {
            std::vector<Uint8> frame;
            const MobileGLResult received = ReceiveEnvelope(*m_transport, frame, kHandshakeTimeoutMs);
            if (received != MOBILEGL_OK) {
                MGLOG_E("MG_Remote client: no Welcome within %u ms (rc=%d)", kHandshakeTimeoutMs,
                        static_cast<int>(received));
                Stop();
                return received;
            }
            const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(frame);
            if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::Welcome) {
                MGLOG_E("MG_Remote client: the server's first control frame is not a verifiable "
                        "Welcome");
                Stop();
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            const ::MobileGL::Wire::Welcome* welcome = envelope->msg_as_Welcome();
            const char* theirStamp = welcome->buildFingerprint() == nullptr
                                         ? nullptr
                                         : welcome->buildFingerprint()->c_str();
            if (welcome->abiFingerprint() != fingerprint) {
                FatalAbiMismatch("struct shapes", fingerprint, welcome->abiFingerprint(),
                                 theirStamp);
            }
            // The four SegmentRefs are what a spawn client MAPS (P6). Under inproc the mapping
            // already exists, so what they are good for here is the cross-check that the two
            // sides agree about the geometry at all - which is the assertion that would
            // otherwise first run in P6, on the day it is expensive to be wrong.
            using Slot = Transport::SessionSegmentSlot;
            const auto agrees = [&](const ::MobileGL::Wire::SegmentRef* ref, Slot slot,
                                    const char* name) {
                if (ref == nullptr || ref->sizeBytes() != server.Shm().AnnouncedSize(slot)) {
                    MGLOG_E("MG_Remote client: Welcome's %s SegmentRef announces %llu bytes, the "
                            "server mapped %llu",
                            name,
                            static_cast<unsigned long long>(ref == nullptr ? 0 : ref->sizeBytes()),
                            static_cast<unsigned long long>(server.Shm().AnnouncedSize(slot)));
                    return false;
                }
                return true;
            };
            if (!agrees(welcome->cmdRing(), Slot::Cmd, "cmd") ||
                !agrees(welcome->stageRing(), Slot::Stage, "stage") ||
                !agrees(welcome->replyPool(), Slot::Reply, "reply") ||
                !agrees(welcome->eventRing(), Slot::Event, "event")) {
                Stop();
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
        }

        // ---- 5. attach to the four segments. Under `inproc` this is the SAME mapping booked
        // under the client role; under `spawn` it becomes ShmSegment::Adopt of the fds the
        // server passed by SCM_RIGHTS, which is why nothing below this line knows which it was.
        const MobileGLResult attached =
            m_shm.AttachInProcess(server.Shm(), Transport::MemoryRole::Client);
        if (attached != MOBILEGL_OK) {
            Stop();
            return attached;
        }

        Transport::RingControl* control = m_shm.CmdControl();
        m_cmd = Transport::RingProducer(control, m_shm.CmdRingBase(), m_shm.CmdRingCapacity(),
                                        Transport::RingCursorSet::Cmd);
        m_stage = Transport::RingProducer(control, m_shm.StageBase(), m_shm.StageCapacity(),
                                          Transport::RingCursorSet::Stage);
        if (!m_cmd.Valid() || !m_stage.Valid()) {
            Stop();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // PeerDoorbell() is the bell the SERVER parks on and this side rings; SelfDoorbell() is
        // this side's own. Which is which is the session's knowledge, not the transport's.
        m_producer.Attach(control, &m_cmd, &m_stage, &m_clientTransport->PeerDoorbell(),
                          &m_clientTransport->SelfDoorbell(), MG_Config::Ipc.SpinUs);

        m_replies = Transport::ReplySlotPool(m_shm.ReplyBase(), m_shm.ReplyBytes(),
                                             m_shm.ReplySlotCount());
        m_events = Transport::EventRingConsumer(m_shm.EventControl(), control,
                                                m_shm.EventRingBase(), m_shm.EventRingCapacity(),
                                                m_shm.EventSegmentBase());
        if (!m_replies.Valid() || !m_events.Valid()) {
            Stop();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        m_barrierArmed = MG_Config::Ipc.VerbBarrier != 0;
        if (!m_barrierArmed) {
            MGLOG_W("MG_Remote client: MOBILEGL_IPC_VERB_BARRIER=0 - this is R-1's NEGATIVE "
                    "CONTROL and is EXPECTED to be red. 31 of the 63 PipeInputs fields are still "
                    "pulled from a live GLContext by the client's residual fill, so a free-running "
                    "queue lets the server read a FUTURE value of them");
        }

        // ---- 6. the client's segment table. IT MUST NOT INSTALL THE PROCESS RESOLVER: there is
        // exactly one gMGPipeSegmentResolver per process, the SERVER role owns it (table 3), and
        // the client never resolves a span at all - it only ever writes Ptr = nullptr (R-2's
        // rule B). Two roles racing on that one inline variable is precisely what the server's
        // InstallProcessResolver asserts against.
        //
        // The Install calls themselves are package w1's and are named-Fatal stubs until w1
        // lands; see ServerSession::Accept for why they are called anyway.
        m_segments.Install(Wire::kSegCmd,
                           Wire::SegmentView{m_shm.CmdRingBase(), m_shm.CmdRingCapacity()});
        m_segments.Install(Wire::kSegStage,
                           Wire::SegmentView{m_shm.StageBase(), m_shm.StageCapacity()});
        m_segments.Install(Wire::kSegReply,
                           Wire::SegmentView{m_shm.ReplyBase(), m_shm.ReplyBytes()});
        m_segments.Install(Wire::kSegEvent,
                           Wire::SegmentView{m_shm.EventSegmentBase(),
                                             m_shm.AnnouncedSize(Transport::SessionSegmentSlot::Event)});
        m_encoder = Wire::PipeWireEncoder(control, &m_cmd, &m_stage, &m_segments);

        // ---- 7. the first CapsSnapshot, if the server had a backend to publish one from.
        if (m_transport->PeekFrameSize() != 0) {
            std::vector<Uint8> frame;
            if (ReceiveEnvelope(*m_transport, frame, 0) == MOBILEGL_OK) {
                const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(frame);
                if (envelope != nullptr &&
                    envelope->msg_type() == ::MobileGL::Wire::CtrlMsg::CapsSnapshot) {
                    // The mirror's Adopt and the two blob DECODERS are c1's and w1's. s1 stops
                    // at "the snapshot arrived and is verifiable": adopting it here would put
                    // the caps mirror's invalidation rule (R-12: a second arrival IS the
                    // invalidation) in two places.
                    MGLOG_I("MG_Remote client: first CapsSnapshot received (%llu bytes); adopting "
                            "it is package c1's CapsMirror::Adopt over package w1's decoders",
                            static_cast<unsigned long long>(frame.size()));
                }
            }
        }

        m_started = true;
        g_active = this;
        LogMemory("handshake");

        // ---- 8. and only now the apply thread. It is package v1's ServerLoop: it names the
        // thread mgl-srv-apply, applies MOBILEGL_IPC_SERVER_AFFINITY and logs the RESOLVED
        // mask. Under `inproc` the client is what starts the server role, which is why this
        // call is here rather than in some server-side main.
        const MobileGLResult running = Server::ServerLoopInstance().Start(server);
        if (running != MOBILEGL_OK) {
            Stop();
            return running;
        }
        return MOBILEGL_OK;
    }

    void ClientSession::Stop() {
        if (!m_started) {
            // Start's own failure paths land here with a half-built session; tear down what
            // exists and leave nothing mapped.
            m_producer.Detach();
            m_shm.Close();
            m_clientTransport.reset();
            m_serverTransport.reset();
            m_transport = nullptr;
            return;
        }

        // TABLE 3's TEARDOWN ORDER, and every step of it is load-bearing.
        //
        // 1. publish and let the server drain. Bounded: a lost record must be a red lane, not
        //    a hung exit.
        Transport::RingControl* control = m_shm.CmdControl();
        if (control != nullptr) {
            const Uint64 submitted = control->submittedSeq.load(std::memory_order_acquire);
            m_producer.PublishAndNotify(submitted);
            if (submitted != 0 &&
                m_producer.WaitForApplied(submitted, kDrainTimeoutMs) != Transport::SessionWait::Reached) {
                MGLOG_E("MG_Remote client: the server did not drain to seq %llu within %u ms; "
                        "tearing down anyway, and anything an emitter still owns is freed below "
                        "AFTER the join, which is what keeps that from being a use-after-free",
                        static_cast<unsigned long long>(submitted), kDrainTimeoutMs);
            }
        }

        // 2. Doorbell::Kill(). THE ONLY thing that can wake an apply thread parked on
        //    kWaitForever (Doorbell.h:211-221): a Notify is consumed by one Park, after which
        //    Doorbell::Wait re-tests a condition nothing published, finds the bell alive and
        //    parks again, forever. InProcessChannel::Close kills both bells.
        if (m_transport != nullptr) {
            m_transport->Shutdown();
        }

        // 3. JOIN, bounded - package v1's ServerLoop::Stop, which also destroys the server's
        //    private BackendObject on that thread before it exits.
        Server::ServerLoopInstance().Stop();

        // 4. and ONLY NOW may anything an emitter owns be released: a var-tail still
        //    referenced by an unapplied record is a use-after-free the join is what prevents.
        LogMemory("teardown");
        m_producer.Detach();
        m_encoder = Wire::PipeWireEncoder();
        m_events = Transport::EventRingConsumer();
        m_replies = Transport::ReplySlotPool();
        m_cmd = Transport::RingProducer();
        m_stage = Transport::RingProducer();
        m_shm.Close();
        Server::ServerSessionInstance().Close();
        m_clientTransport.reset();
        m_serverTransport.reset();
        m_transport = nullptr;
        m_started = false;
        if (g_active == this) {
            g_active = nullptr;
        }
    }

    Wire::PipeWireEncoder& ClientSession::Encoder() { return m_encoder; }

    CapsMirror& ClientSession::Caps() { return CapsMirrorInstance(); }

    // PACKAGE c1's. The barrier's wait and the reply's wait are ONE wait (R-3/R-5), which is
    // what makes a blocking ReadPixels, MapPersistent's decline and the four Bool acceptances
    // cost zero extra round trips - and the client may not re-derive any of those four answers
    // locally. s1 supplies the four primitives it composes from: Encoder(), Producer(),
    // WaitForApplied() and ReadReply().
    Uint64 ClientSession::EmitAndWait(MG_Pipe::MGPWireOp, const void*, Uint64, const void*, Uint64,
                                      void*, Uint64, Int32*) {
        MGP5_C0_STUB("ClientSession::EmitAndWait");
    }

    Bool ClientSession::BarrierArmed() const { return m_barrierArmed; }

    // False, not a Fatal, for both: these are the R-1 mutual-exclusion assertion's two probes,
    // and an assertion helper that aborts when asked is worse than useless. Package c1 gives
    // them real answers when it lands the barrier.
    Bool ClientSession::InBarrierWait() { return false; }
    Bool ClientSession::ApplyThreadIsInsideApplier() { return false; }

    Transport::SessionProducer& ClientSession::Producer() { return m_producer; }

    Transport::SessionWait ClientSession::WaitForApplied(Uint64 seq, Uint32 timeoutMs) {
        return m_producer.WaitForApplied(seq, timeoutMs);
    }

    Bool ClientSession::ReadReply(Uint64 seq, void* outBytes, Uint64 outCapacity, Int32* outStatus,
                                  Uint64* outSize) {
        return m_replies.Read(seq, outBytes, outCapacity, outStatus, outSize);
    }

    Uint32 ClientSession::MaxReplyBytes() const { return m_replies.MaxReplyBytes(); }

    Transport::EventRingConsumer& ClientSession::Events() { return m_events; }

    Transport::RingControl* ClientSession::Control() { return m_shm.CmdControl(); }

    Transport::SessionSegments& ClientSession::Shm() { return m_shm; }

    Transport::ITransport* ClientSession::Control_Plane() { return m_transport; }

    Transport::RoleMemorySample ClientSession::SampleMemory() const {
        return Transport::SampleRoleMemory(Transport::MemoryRole::Client);
    }

    void ClientSession::LogMemory(const char* phase) const {
        Transport::LogRoleMemory(phase, SampleMemory());
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Client
