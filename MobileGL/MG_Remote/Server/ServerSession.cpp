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
#include <MG_Pipe/MGPipeCallbacks.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    // THE THREE FLAG-SPACE COLLISIONS, AS TRIPWIRES RATHER THAN AS A COMMENT.
    //
    // MGPipeCallFlags (MG_Pipe/MGPipe.h:42-54) and RingRecordFlags (Ring.h) are separate spaces
    // that overlap, and three bits mean DIFFERENT things in each. Ring.h's enum carries the
    // table; these are the assertions that break the build if either enum is renumbered, so the
    // collision can never become news again. They live here because this is the nearest .cpp
    // that legally sees both headers - nothing under Transport/ may reach MobileGL/Includes.h.
    static_assert(static_cast<Uint16>(MG_Pipe::kVarTail) == Transport::kRecPad,
                  "MGPipeCallFlags::kVarTail and kRecPad share bit 2: an encoder that copies call "
                  "flags into RingRecordHeader::flags makes every var-tail record read as a wrap "
                  "filler. RingConsumer::Pop requires kind == kRingPadRecordKind as well, which is "
                  "what keeps that from eating the record - do not relax it");
    static_assert(static_cast<Uint16>(MG_Pipe::kHostSpan) == Transport::kRecBorrowSlot,
                  "MGPipeCallFlags::kHostSpan and kRecBorrowSlot share bit 3: a host-span record "
                  "would read as borrowed into the GPU timeline and stop the consumer reclaiming "
                  "ring bytes behind it. SessionConsumer counts and names every sighting");
    static_assert(static_cast<Uint16>(MG_Pipe::kReplySlot) == Transport::kRecVarTail,
                  "MGPipeCallFlags::kReplySlot and kRecVarTail share bit 4");
    static_assert(static_cast<Uint16>(MG_Pipe::kNeedsAck) == Transport::kRecNeedsAck &&
                      static_cast<Uint16>(MG_Pipe::kHasBlob) == Transport::kRecHasBlob,
                  "the two bits that DO mean the same thing in both spaces have drifted apart, "
                  "which is a different and worse problem than the three that collide");

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
            sizes.StageBytes = stageMb * 1024ull * 1024ull;
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

        // ---- P5c ev: the reverse channel's PRODUCER callbacks (CONTRACT-P5C §4.1) --------
        //
        // With an active transport the four reverse entries of gMGPipeCallbacks belong to
        // the SERVER session, never to the client: the backend calls them with exactly the
        // arguments it always passed (a writeback blobref whose Offset IS the backend's own
        // mapped pointer, a range array, a surface info), and what the callback does with
        // the call is Reserve + fill the head + copy the payload + PublishEvents. The host
        // pointer NEVER reaches the wire - what crosses is the inline copy inside the
        // SEG_EVENT record, which is rule B binding the reverse direction exactly as it
        // binds MGHostSpan (R-2).
        //
        // OVERFLOW IS A DEFECT, NOT A DROP (§4.4): all four events are lossless in P5c, so
        // a Reserve that returns nullptr is Fatal{EventRingOverflow}. CountDrop and the
        // eventRingFull latch stay built and stay unused-by-policy; P9 owns the policy.
        [[noreturn]] void FatalEventRingOverflow(const char* eventName, Uint64 payloadBytes) {
            MGLOG_F("MGPipe: Fatal{EventRingOverflow} - the producer of %s could not reserve "
                    "%llu bytes on SEG_EVENT. P5c's events are lossless and a full ring under "
                    "lockstep is a producer burst no measured workload has, so this is a "
                    "defect, not a drop (CONTRACT-P5C §4.4; the drop policy is P9's)",
                    eventName, static_cast<unsigned long long>(payloadBytes));
            std::abort();
        }

        // ---- P5e (ra), CONTRACT-P5E §2.6: the ring stops being a Fatal and becomes a queue --
        //
        // THE FATAL'S PREMISE DIES WITH THE LOCKSTEP. "A full ring is a producer burst no
        // workload has" was true because the client drained at EVERY verb: the ring was empty
        // whenever the server started a record, so filling 256 KiB inside one apply meant a
        // defect. A run-ahead client drains at its waits, which on the steady path are one
        // present apart, so a full ring is now an ordinary backlog and aborting on it would
        // turn the reverse channel's capacity into a workload limit.
        //
        // SO THE PRODUCER BLOCKS. It publishes what is already in the ring and rings the
        // client (a client that has not yet noticed cannot drain), then parks on its own bell
        // until the client's Drained() clears the latch and rings back. The deadlock argument
        // is the contract's, and it needs BOTH halves: the server blocks only here, the client
        // blocks only on watermarks the server advances, and every client park breaks on
        // `eventRingFull` as well (SessionProducer::WaitFor*OrEventBacklog) - so a parked
        // client is always wakeable by the server that is waiting for it.
        //
        // TWO NAMED REFUSALS SURVIVE, and they are the impossible cases rather than the busy
        // ones: a single event larger than the ring can EVER hold (no amount of draining
        // helps), and a wait that ran out of patience (the peer is gone or wedged). A server
        // that does not publish kCapRunAheadApply keeps P5C's Fatal unchanged.
        Bool ServerPublishesRunAhead(const ServerSession& session) {
            if (!session.CallMaskIsSet()) return false;
            return (session.CallMask() & static_cast<Uint64>(MG_Pipe::kCapRunAheadApply)) != 0;
        }

        constexpr Uint32 kEventBacklogWaitMs = 30000;

        void* ReserveEventOrBlock(ServerSession& session, Transport::EventKind kind,
                                  const char* eventName, Uint64 payloadBytes) {
            void* slot = session.Events().Reserve(kind, payloadBytes);
            if (slot != nullptr) return slot;
            if (!ServerPublishesRunAhead(session)) FatalEventRingOverflow(eventName, payloadBytes);

            // Impossible case one: the record does not fit an EMPTY ring. Reserve caps one
            // record at capacity/2 (Ring.h), so no drain can ever make room and blocking would
            // be a hang with a comment on it.
            const Uint64 cap = session.Events().Ring().MaxRecordBytes();
            if (payloadBytes + sizeof(Transport::RingRecordHeader) > cap) {
                MGLOG_F("MGPipe: Fatal{EventRingOverflow} - %s needs %llu bytes and SEG_EVENT "
                        "caps ONE record at %llu (half its capacity). Flow control cannot help: "
                        "no amount of draining makes a record fit a ring that is too small for "
                        "it. Raise MOBILEGL_IPC_EVENT_KB or slice the event at its producer, as "
                        "the writeback path already does",
                        eventName, static_cast<unsigned long long>(payloadBytes),
                        static_cast<unsigned long long>(cap));
                std::abort();
            }

            Transport::RingControl& control = session.Control();
            Transport::Doorbell& bell = session.ConsumerDoorbell();
            const auto drained = [&control] {
                return control.eventRingFull.load(std::memory_order_acquire) == 0;
            };
            for (Uint32 round = 0; round < 2; ++round) {
                // PUBLISH AND RING FIRST. The client cannot drain a ring whose head it has not
                // been shown, and the latch Reserve just set is only useful to a client that
                // gets a bell with it.
                session.PublishEvents();
                if (!drained() && !bell.Wait(control.consumerParked, drained, /*spinUs=*/0,
                                             kEventBacklogWaitMs)) {
                    MGLOG_F("MGPipe: Fatal{EventRingOverflow} - %s waited %u ms for the client "
                            "to drain SEG_EVENT and it never did. Under run-ahead the apply "
                            "thread parks on this ring (CONTRACT-P5E §2.6); a client that does "
                            "not drain is one that is not running",
                            eventName, kEventBacklogWaitMs);
                    std::abort();
                }
                slot = session.Events().Reserve(kind, payloadBytes);
                if (slot != nullptr) return slot;
            }
            // Two full rounds against a ring the record provably fits: something else is
            // producing into it, which on this side is impossible (one producer, the apply
            // thread, by construction).
            MGLOG_F("MGPipe: Fatal{EventRingOverflow} - %s could not reserve %llu bytes on an "
                    "emptied SEG_EVENT. The apply thread is SEG_EVENT's only producer, so a "
                    "drained ring that still refuses a record it fits is a corrupt cursor set",
                    eventName, static_cast<unsigned long long>(payloadBytes));
            std::abort();
        }

        void ServerOnBufferWriteback(MG_Pipe::MGPipeHandle res, Uint64 offset,
                                     MG_Pipe::MGPBlobRef bytes) {
            if (bytes.Seg != MG_Pipe::kMGHostSpanSegNone) {
                // The backend handed over a segment-tagged blobref. The ONLY legal shape at
                // this boundary is the monolith one - Offset is the mapped address, valid
                // for this call - because the segment copy is THIS function's own job.
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"OnBufferWriteback.Seg\"} - the "
                        "writeback producer was handed Seg %u; at the backend boundary the "
                        "blobref names the backend's own mapped bytes (Seg = "
                        "kMGHostSpanSegNone) and the copy into SEG_EVENT is the producer's",
                        bytes.Seg);
                std::abort();
            }
            ServerSession& session = ServerSessionInstance();
            const Uint64 payloadBytes = sizeof(Transport::EventBufferWritebackHead) + bytes.Size;
            void* slot = ReserveEventOrBlock(session, Transport::kEventBufferWriteback,
                                             "kEventBufferWriteback", payloadBytes);
            Transport::EventBufferWritebackHead head{};
            head.Resource = Transport::EventHandle{res.Slot, res.Gen};
            head.Offset = offset;
            head.Size = bytes.Size;
            std::memcpy(slot, &head, sizeof(head));
            if (bytes.Size != 0) {
                std::memcpy(static_cast<Uint8*>(slot) + sizeof(head),
                            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(bytes.Offset)),
                            static_cast<SizeT>(bytes.Size));
            }
            session.PublishEvents();
        }

        void ServerOnGpuWritten(MG_Pipe::MGPipeHandle res, Uint rangeCount,
                                const MG_Pipe::MGPRange* ranges) {
            ServerSession& session = ServerSessionInstance();
            const Uint64 tailBytes = static_cast<Uint64>(rangeCount) * sizeof(Transport::EventRange);
            const Uint64 payloadBytes = sizeof(Transport::EventGpuWrittenHead) + tailBytes;
            void* slot = ReserveEventOrBlock(session, Transport::kEventGpuWritten,
                                             "kEventGpuWritten", payloadBytes);
            Transport::EventGpuWrittenHead head{};
            head.Resource = Transport::EventHandle{res.Slot, res.Gen};
            head.RangeCount = static_cast<std::uint32_t>(rangeCount);
            std::memcpy(slot, &head, sizeof(head));
            if (tailBytes != 0) {
                // EventRange and MGPRange are the same two Uint64s (EventRing.h:61-66 asserts
                // it), so the tail is a plain copy rather than a per-element conversion.
                std::memcpy(static_cast<Uint8*>(slot) + sizeof(head), ranges,
                            static_cast<SizeT>(tailBytes));
            }
            session.PublishEvents();
        }

        void ServerOnSurfaceChanged(const MG_Pipe::MGPSurfaceInfo* info) {
            ServerSession& session = ServerSessionInstance();
            constexpr Uint64 payloadBytes = sizeof(Transport::EventSurfaceChangedHead);
            void* slot = ReserveEventOrBlock(session, Transport::kEventSurfaceChanged,
                                             "kEventSurfaceChanged", payloadBytes);
            Transport::EventSurfaceChangedHead head{};
            if (info != nullptr) {
                head.Width = info->Width;
                head.Height = info->Height;
                head.InternalFormat = info->InternalFormat;
                head.Samples = info->Samples;
                head.Layers = info->Layers;
                head.IsDefault = info->IsDefault;
            }
            std::memcpy(slot, &head, sizeof(head));
            session.PublishEvents();
        }

        // P5f fv: OnGlError now has the same owner and lifetime as the other producers.
        // PostGlError copies message bytes synchronously into the existing FIFO event ring.
        void ServerOnGlError(Uint32 code, const char* message) {
            auto* session = ServerSession::Active();
            if (session == nullptr) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"OnGlError.session-missing\"}");
                std::abort();
            }
            session->PostGlError(code, message);
        }

        // One installer for both roles' tables, so the check exists in exactly one spelling:
        // writing over an entry somebody else claimed is Fatal{RoleViolation,
        // "callback-double-install"} - MGPipeCallbacks' "never over an entry a backend
        // already claimed" comment, made a check (CONTRACT-P5C §4.1). Finding OUR OWN
        // function there is the idempotent repeat, not a second installation.
        template <typename Fn>
        void InstallReverseCallback(Fn& entry, Fn producer) {
            if (entry != nullptr && entry != producer) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"callback-double-install\"} - a reverse "
                        "MGPipeCallbacks entry is already claimed by a different function. With "
                        "an active transport the four reverse entries are the server "
                        "session's producers; the client installs them under monolith only");
                std::abort();
            }
            entry = producer;
        }

        std::atomic<ServerSession*> g_active{nullptr};
        // One callback table and one process segment resolver: an in-progress Accept
        // reserves their owner too. CAS closes the race between distinct session objects
        // without making concurrent sessions a supported mode.
        std::atomic<ServerSession*> g_sessionOwner{nullptr};

    } // namespace

    ServerSession& ServerSessionInstance() {
        // Leak at exit, deliberately and per ID-8: frontend destructors reach pipe and backend
        // state from exit handlers, and a session destroyed before them would be a
        // use-after-free rather than a tidy teardown.
        static ServerSession* instance = new ServerSession{};
        return *instance;
    }

    ServerSession* ServerSession::Active() { return g_active.load(std::memory_order_acquire); }

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
        ServerSession* expectedOwner = nullptr;
        if (!g_sessionOwner.compare_exchange_strong(expectedOwner, this,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expectedOwner == this) return MOBILEGL_ERR_INVALID_ARGUMENT;
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"callback-double-install\"} - another "
                    "ServerSession already owns or is accepting the process reverse channel");
            std::abort();
        }
        struct ReleaseFailedClaim {
            ServerSession* self;
            ~ReleaseFailedClaim() {
                if (!self->Accepted()) {
                    ServerSession* expected = self;
                    g_sessionOwner.compare_exchange_strong(expected, nullptr,
                        std::memory_order_release, std::memory_order_relaxed);
                }
            }
        } releaseOnFailure{this};
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
                           Wire::SegmentView{m_shm.StageBase(), m_shm.StageBytes()});
        m_segments.Install(Wire::kSegReply,
                           Wire::SegmentView{m_shm.ReplyBase(), m_shm.ReplyBytes()});
        m_segments.Install(Wire::kSegEvent, Wire::SegmentView{m_shm.EventSegmentBase(),
                                                              m_shm.AnnouncedSize(
                                                                  Transport::SessionSegmentSlot::Event)});
        m_segments.InstallProcessResolver();

        m_accepted = true;

        // ---- P5c ev: the four reverse-channel producers are THIS session's (CONTRACT-P5C
        // §4.1), installed before the apply thread can apply a record that produces one and
        // uninstalled at Close after the join. Under monolith these entries are the
        // client's (MGPipeInstallClientResourceCallbacks, monolith-only now); a session is
        // by definition not monolith, so finding one of them claimed here is the double
        // installation the check exists to name.
        InstallReverseCallback(MG_Pipe::gMGPipeCallbacks.OnGlError, &ServerOnGlError);
        InstallReverseCallback(MG_Pipe::gMGPipeCallbacks.OnBufferWriteback,
                               &ServerOnBufferWriteback);
        InstallReverseCallback(MG_Pipe::gMGPipeCallbacks.OnGpuWritten, &ServerOnGpuWritten);
        InstallReverseCallback(MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged,
                               &ServerOnSurfaceChanged);
        g_active.store(this, std::memory_order_release);

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
        ServerSession* expectedActive = this;
        g_active.compare_exchange_strong(expectedActive, nullptr,
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (m_accepted) {
            // Uninstall AFTER the apply thread has joined, never before: a record still in
            // flight can still resolve a segment offset (table 3's fourth column).
            Wire::SegmentTable::UninstallProcessResolver();
            // The reverse-channel producers go with the session that owns them - release
            // only the entries that are still OURS, never one a later owner installed.
            if (MG_Pipe::gMGPipeCallbacks.OnGlError == &ServerOnGlError) {
                MG_Pipe::gMGPipeCallbacks.OnGlError = nullptr;
            }
            if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback == &ServerOnBufferWriteback) {
                MG_Pipe::gMGPipeCallbacks.OnBufferWriteback = nullptr;
            }
            if (MG_Pipe::gMGPipeCallbacks.OnGpuWritten == &ServerOnGpuWritten) {
                MG_Pipe::gMGPipeCallbacks.OnGpuWritten = nullptr;
            }
            if (MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged == &ServerOnSurfaceChanged) {
                MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged = nullptr;
            }
        }
        m_consumer.Detach();
        m_commands = Transport::RingConsumer();
        m_events = Transport::EventRingProducer();
        m_replies = ReplyPool();
        m_shm.Close();
        m_transport = nullptr;
        m_accepted = false;
        ServerSession* expectedOwner = this;
        g_sessionOwner.compare_exchange_strong(expectedOwner, nullptr,
            std::memory_order_release, std::memory_order_relaxed);
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

    void ServerSession::PostGlError(Uint32 code, const char* message) {
        if (!m_accepted) {
            // The same answer PipeInputs::RecordError's own no-live-context arm gives: a
            // dropped driver error is loud, never silent.
            MGLOG_E_ONCE("MG_Remote server: PostGlError (code %u) before Accept - the error is "
                         "dropped, because there is no SEG_EVENT to carry it on",
                         static_cast<unsigned>(code));
            return;
        }
        // The message rides INLINE, NUL-terminated, MessageBytes = strlen + 1 (CONTRACT-P5C
        // §1), truncated to the cap at the producer - which is here, and is why the cap is a
        // constant of the wire header rather than a negotiated value.
        SizeT length = message == nullptr ? 0 : std::strlen(message);
        if (length >= Transport::kEventGlErrorMaxMessageBytes) {
            length = Transport::kEventGlErrorMaxMessageBytes - 1;
        }
        const Uint32 messageBytes = static_cast<Uint32>(length) + 1;
        const Uint64 payloadBytes = sizeof(Transport::EventGlErrorHead) + messageBytes;
        void* slot = ReserveEventOrBlock(*this, Transport::kEventGlError, "kEventGlError",
                                         payloadBytes);
        Transport::EventGlErrorHead head{};
        head.Code = code;
        head.MessageBytes = messageBytes;
        std::memcpy(slot, &head, sizeof(head));
        auto* tail = reinterpret_cast<char*>(static_cast<Uint8*>(slot) + sizeof(head));
        if (length != 0) {
            std::memcpy(tail, message, length);
        }
        tail[length] = '\0';
        PublishEvents();
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
