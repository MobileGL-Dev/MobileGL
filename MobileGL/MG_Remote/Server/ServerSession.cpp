// MobileGL - MobileGL/MG_Remote/Server/ServerSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package s1: the server half of a session.

#include "ServerSession.h"

#include "../CapsCodec.h"
#include "../Protocol/generated/protocol_generated.h"
#include "../Transport/InProcessTransport.h"

#include <Config.h>
#include <MGGitHash.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    namespace {

        // A control-plane frame is small by construction (ITransport.h:56-58: bulk bytes
        // belong in shm, never here), so one stack-free vector sized from PeekFrameSize is
        // the whole reader. The BUFFER_TOO_SMALL half of ReceiveFrame's contract is what
        // makes the two-step safe: a short buffer leaves the message queued.
        MobileGLResult ReceiveEnvelope(Transport::ITransport& transport, std::vector<Uint8>& out,
                                       Uint32 timeoutMs) {
            Uint64 size = 0;
            MobileGLMutableByteSpan empty{nullptr, 0};
            const MobileGLResult probe = transport.ReceiveFrame(empty, &size, timeoutMs);
            if (probe != MOBILEGL_ERR_BUFFER_TOO_SMALL) {
                // OK with a zero-size message, or a real failure. A zero-length control
                // frame is not a legal CtrlEnvelope either way.
                return probe == MOBILEGL_OK ? MOBILEGL_ERR_PROTOCOL_MISMATCH : probe;
            }
            out.resize(static_cast<SizeT>(size));
            MobileGLMutableByteSpan span{out.data(), out.size()};
            return transport.ReceiveFrame(span, &size, 0);
        }

        MobileGLResult SendEnvelope(Transport::ITransport& transport,
                                    ::flatbuffers::FlatBufferBuilder& builder) {
            return transport.SendFrame(
                MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()});
        }

        // Every message from the peer is verified before a single field is read: the control
        // plane is parsed from another process's memory (P6) and from another role's (P5).
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
                                           const char* ourStamp, const char* theirStamp) {
            // NEVER a downgrade. Every alternative to aborting here reads one struct as
            // another - MGPCaps has only a compositional size assertion because
            // DynamicBackendParameters still carries SizeT, so a peer built from a different
            // tree hands over a caps block whose members are at different offsets and whose
            // bytes are all individually plausible.
            MGLOG_F("MGPipe: Fatal{AbiMismatch, \"%s\"} ours=%llu theirs=%llu ourBuild=%s "
                    "theirBuild=%s - the two peers were not built from the same struct shapes, "
                    "and there is no downgrade path: the caps block's size is ABI-dependent "
                    "(MGPipeTypes.h:145-146) and every field past the first difference would be "
                    "read at the wrong offset",
                    what, static_cast<unsigned long long>(ours),
                    static_cast<unsigned long long>(theirs),
                    ourStamp == nullptr ? "?" : ourStamp, theirStamp == nullptr ? "?" : theirStamp);
            std::abort();
        }

        // MOBILEGL_IPC_RING_MB / MOBILEGL_IPC_STAGE_MB, actually applied.
        //
        // The first version of this file called this only `if (m_sizes.CmdBytes == 0)`, and
        // SessionSegmentSizes has a default member initialiser of 8 MiB, so the condition was
        // never true and the whole function was dead: ConfigLoader parsed both knobs, echoed
        // them into the config line, and the session mapped 8/32 MiB regardless. Config.h's
        // own comment four lines above the declaration is the statement of that bug - "an
        // environment variable that nothing parses is indistinguishable from one that is
        // parsed and ignored" - and a knob that REPORTS a value it does not use is worse,
        // because it makes every measurement taken with it a lie.
        Transport::SessionSegmentSizes SizesFromConfig() {
            Transport::SessionSegmentSizes sizes;
#if MOBILEGL_BUILD_DISAGGREGATED
            const Uint64 ringMb = MG_Config::Ipc.RingMb == 0 ? 8u : MG_Config::Ipc.RingMb;
            const Uint64 stageMb = MG_Config::Ipc.StageMb == 0 ? 32u : MG_Config::Ipc.StageMb;
            sizes.CmdRingBytes = ringMb * 1024ull * 1024ull;
            sizes.StageRingBytes = stageMb * 1024ull * 1024ull;
#endif
            return sizes;
        }

        Uint32 SpinUsFromConfig() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.SpinUs;
#else
            return Transport::kDefaultSpinUs;
#endif
        }

        // The handshake's own deadline. Bounded rather than kWaitForever on purpose: a
        // bring-up that never answers must be a red lane, not a wedged CI job - the same
        // reason InProcessTransportTest.cpp:344 bounds its join at five seconds.
        constexpr Uint32 kHandshakeTimeoutMs = 5000;

        // THERE IS NO DERIVATION OF CallMask, BY RULING. See ServerSession.h's block on
        // SetCapabilityBits / SetConsumedSubsystems for why the one that used to be here was
        // the phase's marquee defect committed from the server's side.
        [[noreturn]] void FatalUnsetCallMask(Bool capBitsSet, Bool consumedSet) {
            MGLOG_F("MGPipe: Fatal{UnsetCallMask} - %s%s%s was never set on this ServerSession, "
                    "and there is no default: a guessed consumer mask makes the client's R-8 "
                    "liveness gates answer from a server-side fact the server never stated. The "
                    "client would stop emitting whole record families, clear its dirty flags on "
                    "acceptance anyway, and the lane would go green with the uploads lost "
                    "(ID-39, reflected). Call SetConsumedSubsystems() and SetCapabilityBits() "
                    "before Accept(); SetCapabilityBits(0) is a legitimate explicit answer",
                    capBitsSet ? "" : "SetCapabilityBits",
                    (!capBitsSet && !consumedSet) ? " and " : "",
                    consumedSet ? "" : "SetConsumedSubsystems");
            std::abort();
        }

        ServerSession* g_active = nullptr;

    } // namespace

    ServerSession& ServerSessionInstance() {
        // Leak at exit, deliberately and per ID-8: frontend destructors reach pipe and backend
        // state from exit handlers, and a session destroyed before them would be a
        // use-after-free rather than a tidy teardown.
        static ServerSession* instance = new ServerSession{};
        return *instance;
    }

    ServerSession* ServerSession::Active() { return g_active; }

    ServerSession::~ServerSession() { Close(); }

    void ServerSession::SetSegmentSizes(const Transport::SessionSegmentSizes& sizes) {
        if (m_accepted) {
            MGLOG_E("MG_Remote server: SetSegmentSizes after Accept is ignored - the geometry is "
                    "already on the wire in Welcome and the peer has mapped it");
            return;
        }
        m_sizes = sizes;
        m_sizesSet = true;
    }

    void ServerSession::SetBackend(MG_Backend::BackendObject* backend) { m_backend = backend; }

    void ServerSession::SetCapabilityBits(Uint64 capBits) {
        m_capBits = capBits;
        m_capBitsSet = true;
    }

    void ServerSession::SetConsumedSubsystems(Uint64 subsystemMask) {
        m_consumedSubsystems = subsystemMask;
        m_consumedSet = true;
    }

    Bool ServerSession::CallMaskIsSet() const { return m_capBitsSet && m_consumedSet; }

    Uint64 ServerSession::CallMask() const {
        if (!CallMaskIsSet()) {
            FatalUnsetCallMask(m_capBitsSet, m_consumedSet);
        }
        return m_capBits | MGCapsConsumerBits(m_consumedSubsystems);
    }

    Bool ServerSession::Accepted() const { return m_accepted; }

    MobileGLResult ServerSession::Accept(Transport::ITransport& transport) {
        if (m_accepted) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_transport = &transport;
        // MOBILEGL_IPC_RING_MB / _STAGE_MB unless SetSegmentSizes overrode them. Unconditional
        // on purpose - see SizesFromConfig.
        if (!m_sizesSet) {
            m_sizes = SizesFromConfig();
        }

        // ---- 1/2. the ABI assertion, BEFORE a single record is decoded and before a byte of
        // shared memory exists. Its whole purpose is to refuse to interpret the peer's bytes.
        std::vector<Uint8> frame;
        const MobileGLResult received = ReceiveEnvelope(transport, frame, kHandshakeTimeoutMs);
        if (received != MOBILEGL_OK) {
            MGLOG_E("MG_Remote server: no Hello within %u ms (rc=%d)", kHandshakeTimeoutMs,
                    static_cast<int>(received));
            return received;
        }
        const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(frame);
        // msg_as_Hello() IS PART OF THE GUARD, not a consequence of it. FlatBuffers'
        // Verifier::VerifyTable is `return !table || table->Verify(*this)`, so a NULL union
        // member passes verification: a 24-byte frame verifies, carries the identifier,
        // reports msg_type() == Hello and returns nullptr from msg_as_Hello(). A malformed
        // frame has to be refused, never dereferenced.
        if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::Hello ||
            envelope->msg_as_Hello() == nullptr) {
            MGLOG_E("MG_Remote server: the first control frame is not a verifiable Hello");
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        const ::MobileGL::Wire::Hello* hello = envelope->msg_as_Hello();
        const char* theirStamp =
            hello->buildFingerprint() == nullptr ? nullptr : hello->buildFingerprint()->c_str();

        if (hello->abiMajor() != static_cast<Uint32>(MOBILEGL_PROTOCOL_ABI_MAJOR) ||
            hello->abiMinor() != static_cast<Uint32>(MOBILEGL_PROTOCOL_ABI_MINOR)) {
            FatalAbiMismatch("protocol version",
                             MOBILEGL_ABI_VERSION(MOBILEGL_PROTOCOL_ABI_MAJOR,
                                                  MOBILEGL_PROTOCOL_ABI_MINOR),
                             MOBILEGL_ABI_VERSION(hello->abiMajor(), hello->abiMinor()),
                             GIT_COMMIT_HASH_SHORT, theirStamp);
        }
        const Uint64 ourFingerprint = CapsAbiFingerprint();
        if (hello->abiFingerprint() != ourFingerprint) {
            FatalAbiMismatch("struct shapes", ourFingerprint, hello->abiFingerprint(),
                             GIT_COMMIT_HASH_SHORT, theirStamp);
        }

        // ---- 3. the four segments, both control pages, the rings.
        const MobileGLResult created = m_shm.Create(m_sizes, Transport::MemoryRole::Server);
        if (created != MOBILEGL_OK) {
            return created;
        }

        Transport::RingControl* control = m_shm.CmdControl();
        m_commands = Transport::RingConsumer(control, m_shm.CmdRingBase(), m_shm.CmdRingCapacity(),
                                             Transport::RingCursorSet::Cmd);
        if (!m_commands.Valid()) {
            Close();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_consumer.Attach(control, &m_commands, &ProducerDoorbell(), &ConsumerDoorbell(),
                          SpinUsFromConfig());

        {
            Transport::ReplySlotPool pool(m_shm.ReplyBase(), m_shm.ReplyBytes(),
                                          m_shm.ReplySlotCount());
            if (!pool.Valid()) {
                Close();
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            // A stale stamp from a previous session must never read as this session's answer.
            pool.Clear();
            m_replies = ReplyPool(m_shm.ReplyBase(), m_shm.ReplyBytes(), m_shm.ReplySlotCount(),
                                  pool.SlotBytes());
        }

        m_events = Transport::EventRingProducer(m_shm.EventControl(), control, m_shm.EventRingBase(),
                                                m_shm.EventRingCapacity());
        m_applier = PipeApplier(&m_segments, &m_replies);

        // ---- 4. Welcome: the four SegmentRefs, plus this side's half of the ABI statement.
        {
            ::flatbuffers::FlatBufferBuilder builder(1024);
            using Slot = Transport::SessionSegmentSlot;
            const auto segmentRef = [&](Uint32 id, ::MobileGL::Wire::SegmentKind kind, Slot slot) {
                return ::MobileGL::Wire::CreateSegmentRefDirect(builder, id, kind,
                                                                m_shm.AnnouncedSize(slot),
                                                                m_shm.AnnouncedName(slot));
            };
            auto cmd = segmentRef(1, ::MobileGL::Wire::SegmentKind::Cmd, Slot::Cmd);
            auto stage = segmentRef(2, ::MobileGL::Wire::SegmentKind::Stage, Slot::Stage);
            auto reply = segmentRef(3, ::MobileGL::Wire::SegmentKind::Reply, Slot::Reply);
            auto event = segmentRef(4, ::MobileGL::Wire::SegmentKind::Event, Slot::Event);
            auto stamp = builder.CreateString(GIT_COMMIT_HASH_SHORT);
            auto welcome = ::MobileGL::Wire::CreateWelcome(
                builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                static_cast<Uint32>(hello->pid()), cmd, stage, reply, event, stamp, ourFingerprint);
            auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
                builder, ::MobileGL::Wire::CtrlMsg::Welcome, welcome.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
            const MobileGLResult sent = SendEnvelope(transport, builder);
            if (sent != MOBILEGL_OK) {
                Close();
                return sent;
            }
        }

        // ---- 5. the segment table and the ONE process-wide resolver.
        //
        // Table 3's ruling: gMGPipeSegmentResolver is a plain non-atomic inline variable and
        // there is exactly one per process, so the SERVER role installs it and the client never
        // resolves a span at all - it only ever writes Ptr = nullptr. Installed BEFORE the apply
        // thread starts, uninstalled after the join and never before.
        //
        // Both calls are package w1's (Wire/PipeWireCodec.cpp) and are named-Fatal stubs until
        // w1 lands. That is deliberate and is where the bring-up currently stops: a session
        // that skipped them and carried on would be a decoder with no segments, which is the
        // "split lane ran monolith and went green" shape.
        m_segments.Install(Wire::kSegCmd,
                           Wire::SegmentView{m_shm.CmdRingBase(), m_shm.CmdRingCapacity()});
        m_segments.Install(Wire::kSegStage,
                           Wire::SegmentView{m_shm.StageBase(), m_shm.StageCapacity()});
        m_segments.Install(Wire::kSegReply,
                           Wire::SegmentView{m_shm.ReplyBase(), m_shm.ReplyBytes()});
        m_segments.Install(Wire::kSegEvent, Wire::SegmentView{m_shm.EventSegmentBase(),
                                                              m_shm.AnnouncedSize(
                                                                  Transport::SessionSegmentSlot::Event)});
        m_segments.InstallProcessResolver();

        m_accepted = true;
        g_active = this;
        LogMemory("accept");

        if (!CallMaskIsSet()) {
            // Not fatal HERE, because a session with no backend legitimately publishes no
            // snapshot at all and the mask is only needed by one. It becomes
            // Fatal{UnsetCallMask} the moment PublishCapsSnapshot asks for it, which is the
            // first thing that would put a guess on the wire.
            MGLOG_W("MG_Remote server: accepted with no CallMask - SetConsumedSubsystems() and/or "
                    "SetCapabilityBits() were never called. There is no default and there will be "
                    "no guess: the first CapsSnapshot will abort instead");
        }

        // ---- 6. the first CapsSnapshot, if there is a backend to take it from.
        if (m_backend != nullptr) {
            const MobileGLResult published = PublishCapsSnapshot();
            if (published != MOBILEGL_OK) {
                return published;
            }
        } else {
            MGLOG_W("MG_Remote server: accepted with NO backend, so the first CapsSnapshot is "
                    "deferred. Call SetBackend() then PublishCapsSnapshot(). A client that emits "
                    "before the snapshot arrives reads a placeholder caps mirror");
        }
        return MOBILEGL_OK;
    }

    MobileGLResult ServerSession::PublishCapsSnapshot() {
        if (!m_accepted || m_transport == nullptr) {
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        if (m_backend == nullptr) {
            MGLOG_E("MG_Remote server: PublishCapsSnapshot with no backend");
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }

        // The two blob codecs are package w1's (CapsCodec.h). They are named-Fatal stubs
        // today; nothing here may substitute a memcpy for them, because both structures hold
        // Vectors and Strings and a memcpy of either crosses a host pointer (R-2's rule B).
        Vector<Uint8> formats;
        Vector<Uint8> renderer;
        if (!EncodeFormatCapabilities(m_backend->GetFormatCapabilities(), formats)) {
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        if (!EncodeRendererInfo(m_backend->GetRendererInfo(), renderer)) {
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }

        const MG_Backend::DynamicBackendParameters& dynamic = m_backend->GetDynamicParameters();
        const auto* dynamicBytes = reinterpret_cast<const Uint8*>(&dynamic);

        // R-8/C-4: bits 32..47 of CallMask are the CONSUMER MASK, and this is the only place
        // they are produced.
        const Uint64 callMask = CallMask();

        ::flatbuffers::FlatBufferBuilder builder(4096);
        auto dynamicVector =
            builder.CreateVector(dynamicBytes, static_cast<::flatbuffers::uoffset_t>(sizeof(dynamic)));
        auto rendererVector = builder.CreateVector(renderer.data(), renderer.size());
        auto formatsVector = builder.CreateVector(formats.data(), formats.size());
        const RendererInfo& info = m_backend->GetRendererInfo();
        auto apiVersion = builder.CreateString(info.RendererGLInfo.TargetGLVersion.toString());
        auto snapshot = ::MobileGL::Wire::CreateCapsSnapshot(
            builder, dynamicVector, rendererVector, formatsVector, /*extensions=*/0, apiVersion,
            callMask, static_cast<Uint32>(m_backend->GetBackendType()));
        auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
            builder, ::MobileGL::Wire::CtrlMsg::CapsSnapshot, snapshot.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
        return SendEnvelope(*m_transport, builder);
    }

    void ServerSession::Close() {
        if (g_active == this) {
            g_active = nullptr;
        }
        if (m_accepted) {
            // Uninstall AFTER the apply thread has joined, never before: a record still in
            // flight can still resolve a segment offset (table 3's fourth column).
            Wire::SegmentTable::UninstallProcessResolver();
        }
        m_consumer.Detach();
        m_commands = Transport::RingConsumer();
        m_events = Transport::EventRingProducer();
        m_replies = ReplyPool();
        m_shm.Close();
        m_transport = nullptr;
        m_accepted = false;
    }

    Transport::RingConsumer& ServerSession::CommandRing() { return m_commands; }

    Transport::RingControl& ServerSession::Control() {
        Transport::RingControl* control = m_shm.CmdControl();
        if (control == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ServerSession::Control\"} - the control "
                    "page does not exist until Accept() has mapped SEG_CMD");
            std::abort();
        }
        return *control;
    }

    Wire::SegmentTable& ServerSession::Segments() { return m_segments; }
    PipeApplier& ServerSession::Applier() { return m_applier; }
    ReplyPool& ServerSession::Replies() { return m_replies; }

    // The bell the apply thread parks on. On the server endpoint of an InProcessTransport that
    // is SelfDoorbell(); the client reaches the same bell through its own PeerDoorbell().
    Transport::Doorbell& ServerSession::ConsumerDoorbell() {
        if (m_transport == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ServerSession::ConsumerDoorbell\"} - no "
                    "transport; Accept() has not run");
            std::abort();
        }
        if (m_transport->Role() == Transport::TransportRole::InProcess) {
            return static_cast<Transport::InProcessTransport*>(m_transport)->SelfDoorbell();
        }
        // P6: SocketTransport's pair. The accessors stay off ITransport by ruling (contract
        // §3.9) precisely so that this stays one switch in one file rather than two virtuals
        // every transport has to invent a home for.
        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"ServerSession::ConsumerDoorbell\"} - transport "
                "role %u has no doorbell pair yet; that is P6's SocketTransport",
                static_cast<unsigned>(m_transport->Role()));
        std::abort();
    }

    Transport::Doorbell& ServerSession::ProducerDoorbell() {
        if (m_transport == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ServerSession::ProducerDoorbell\"} - no "
                    "transport; Accept() has not run");
            std::abort();
        }
        if (m_transport->Role() == Transport::TransportRole::InProcess) {
            return static_cast<Transport::InProcessTransport*>(m_transport)->PeerDoorbell();
        }
        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"ServerSession::ProducerDoorbell\"} - transport "
                "role %u has no doorbell pair yet; that is P6's SocketTransport",
                static_cast<unsigned>(m_transport->Role()));
        std::abort();
    }

    Transport::SessionSegments& ServerSession::Shm() { return m_shm; }
    Transport::SessionConsumer& ServerSession::Consumer() { return m_consumer; }
    Transport::EventRingProducer& ServerSession::Events() { return m_events; }
    Transport::ITransport* ServerSession::Control_Plane() { return m_transport; }

    void ServerSession::PublishEvents() {
        if (!m_accepted) {
            return;
        }
        // Publish, THEN ring - the same order as the forward direction, and the session picks
        // the bell so that no caller can pair the right ring with the wrong flag.
        m_events.Ring().Publish();
        m_consumer.NotifyClient();
    }

    // Both of these advance AND ring, through SessionConsumer. The free functions in namespace
    // Watermark do not ring: a client parked in WaitForPresentAck(kWaitForever) needs the pair.
    void ServerSession::AdvanceCompletedFrame(Uint64 serial) {
        if (!m_accepted) {
            return;
        }
        m_consumer.CompleteFrame(serial);
    }

    void ServerSession::ReturnPresentCredit(Uint64 serial) {
        if (!m_accepted) {
            return;
        }
        m_consumer.ReturnPresentCredit(serial);
    }

    Transport::RoleMemorySample ServerSession::SampleMemory() const {
        return Transport::SampleRoleMemory(Transport::MemoryRole::Server);
    }

    void ServerSession::LogMemory(const char* phase) const {
        Transport::LogRoleMemory(phase, SampleMemory());
    }

} // namespace MobileGL::MG_Remote::Server
