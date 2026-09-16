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
#include "WireTables.h"

#include <MGGitHash.h>
#include <MG_Pipe/MGPipeCallbacks.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
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

        // Guarded the same way ServerSession's helpers are: MG_Config::Ipc only exists
        // behind MOBILEGL_BUILD_DISAGGREGATED (Config.h), and this file is only compiled
        // there today - but the guard is what keeps that true if the source list ever
        // changes, and an unguarded read would be a compile error nobody could read.
        Uint32 SpinUsFromConfig() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.SpinUs;
#else
            return Transport::kDefaultSpinUs;
#endif
        }

        Bool VerbBarrierFromConfig() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.VerbBarrier != 0;
#else
            return true;
#endif
        }

        // ---- c1: the barrier's two flags ------------------------------------------------
        //
        // thread_local for the client's own "am I waiting", a shared atomic for "is the apply
        // thread inside the applier" - see ClientSession::InBarrierWait's note.
        thread_local Bool g_inBarrierWait = false;
        std::atomic<Bool> g_applyThreadInsideApplier{false};

        struct BarrierWaitScope {
            BarrierWaitScope() { g_inBarrierWait = true; }
            ~BarrierWaitScope() { g_inBarrierWait = false; }
            BarrierWaitScope(const BarrierWaitScope&) = delete;
            BarrierWaitScope& operator=(const BarrierWaitScope&) = delete;
        };

        // BOUNDED, AND THE BOUND IS GENEROUS RATHER THAN TIGHT. The barrier is a correctness
        // device, not a watchdog: a slow readback on a software rasterizer is a legitimate
        // second-scale wait, while a lost record never completes at all. 30 s separates the two
        // without turning a loaded CI machine into a red lane, and the Fatal names the seq.
        constexpr Uint32 kBarrierTimeoutMs = 30000;
        // How many queued control frames one pump will drain. A backlog deeper than this is a
        // finding, not a steady state.
        constexpr Uint32 kMaxControlFramesPerPump = 16;

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

        // ---- c1: the reverse channel's reading end ---------------------------------------
        //
        // R-12's three: OnBufferWriteback (#3), OnGpuWritten (#2), OnSurfaceChanged (#7).
        // Drained BY THE GL THREAD BETWEEN VERBS, which under the barrier means immediately
        // after appliedSeq reaches this record - the one moment at which the apply thread is
        // known not to be inside the applier.
        //
        // THE BYTES LIVE IN THE RING ITSELF, so Drained() is called only after every payload
        // pointer popped here has been consumed: retiring earlier is R-11's violation one level
        // down (EventRing.h:168-171 says so in as many words).
        Uint32 DrainEventRing(Transport::EventRingConsumer& events) {
            if (!events.Valid()) return 0;
            Uint32 delivered = 0;
            Transport::RingRecordView view{};
            Bool corrupt = false;
            while (events.Pop(view, &corrupt)) {
                if (corrupt) {
                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"event-ring\"} - the reverse "
                            "channel's record stream is corrupt");
                    std::abort();
                }
                switch (view.kind) {
                case Transport::kEventBufferWriteback: {
                    if (view.payloadSize < sizeof(Transport::EventBufferWritebackHead)) break;
                    const auto* head =
                        static_cast<const Transport::EventBufferWritebackHead*>(view.payload);
                    const void* bytes = static_cast<const Uint8*>(view.payload) + sizeof(*head);
                    if (view.payloadSize - sizeof(*head) < head->Size) {
                        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"buffer-writeback\"} - the "
                                "head declares %llu inline bytes and the record carries %llu",
                                static_cast<unsigned long long>(head->Size),
                                static_cast<unsigned long long>(view.payloadSize - sizeof(*head)));
                        std::abort();
                    }
                    if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback != nullptr) {
                        // The blobref names SEG_EVENT and the IN-SEGMENT offset of those inline
                        // bytes - never a host address (R-2's rule B), which is the whole reason
                        // EventRingConsumer exposes OffsetInSegment at all.
                        MG_Pipe::MGPBlobRef blob{};
                        blob.Seg = Wire::kSegEvent;
                        blob.Offset = events.OffsetInSegment(bytes);
                        blob.Size = head->Size;
                        MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(
                            MG_Pipe::MGPipeHandle{head->Resource.Slot, head->Resource.Gen},
                            head->Offset, blob);
                        ++delivered;
                    }
                    break;
                }
                case Transport::kEventGpuWritten: {
                    if (view.payloadSize < sizeof(Transport::EventGpuWrittenHead)) break;
                    const auto* head =
                        static_cast<const Transport::EventGpuWrittenHead*>(view.payload);
                    const Uint64 tail = view.payloadSize - sizeof(*head);
                    if (tail / sizeof(Transport::EventRange) < head->RangeCount) {
                        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"gpu-written\"} - RangeCount "
                                "%u does not fit the record's %llu tail bytes",
                                static_cast<unsigned>(head->RangeCount),
                                static_cast<unsigned long long>(tail));
                        std::abort();
                    }
                    if (MG_Pipe::gMGPipeCallbacks.OnGpuWritten != nullptr) {
                        // EventRange and MGPRange are the same two Uint64s (EventRing.h:61-66
                        // asserts it), so the tail is handed over as-is rather than copied into
                        // a second array a later reader could get out of step with.
                        const auto* ranges = reinterpret_cast<const MG_Pipe::MGPRange*>(
                            static_cast<const Uint8*>(view.payload) + sizeof(*head));
                        MG_Pipe::gMGPipeCallbacks.OnGpuWritten(
                            MG_Pipe::MGPipeHandle{head->Resource.Slot, head->Resource.Gen},
                            static_cast<Uint>(head->RangeCount), ranges);
                        ++delivered;
                    }
                    break;
                }
                case Transport::kEventSurfaceChanged: {
                    if (view.payloadSize < sizeof(Transport::EventSurfaceChangedHead)) break;
                    if (MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged != nullptr) {
                        const auto* head =
                            static_cast<const Transport::EventSurfaceChangedHead*>(view.payload);
                        MG_Pipe::MGPSurfaceInfo info{};
                        info.Width = head->Width;
                        info.Height = head->Height;
                        info.InternalFormat = head->InternalFormat;
                        info.Samples = head->Samples;
                        info.Layers = head->Layers;
                        info.IsDefault = head->IsDefault;
                        MG_Pipe::gMGPipeCallbacks.OnSurfaceChanged(&info);
                        ++delivered;
                    }
                    break;
                }
                default:
                    MGLOG_W("MG_Remote client: reverse-channel record kind %u is not consumed in "
                            "P5 (R-12 takes three and a half of the ten callbacks)",
                            static_cast<unsigned>(view.kind));
                    break;
                }
            }
            // AND ONLY NOW. Every payload pointer above has been consumed.
            events.Drained();
            return delivered;
        }

        // ---- c1: the CapsSnapshot -> CapsMirror adoption, in ONE place -------------------
        //
        // Every field of the snapshot has exactly one reader, and a field that fails to decode
        // is a REFUSAL rather than a partial adopt: CompileEnv.cpp:123 copies the whole
        // DynamicBackendParameters struct into the compile env, so a mirror that adopted three
        // of four members would put the fourth's default into a shader fingerprint.
        Bool AdoptCapsSnapshot(const ::MobileGL::Wire::CapsSnapshot* snapshot) {
            if (snapshot == nullptr) return false;

            MG_Pipe::MGPCaps caps{};
            const auto* dynamicBytes = snapshot->dynamicParameters();
            if (dynamicBytes == nullptr || dynamicBytes->size() != sizeof(caps.Dynamic)) {
                // The Hello/Welcome fingerprint already asserted both peers agree on
                // sizeof(DynamicBackendParameters), so a disagreement HERE is a corrupt frame
                // rather than an ABI skew - which is why it is a refusal and not FatalAbiMismatch.
                MGLOG_E("MG_Remote client: CapsSnapshot carries %llu dynamic bytes, this build's "
                        "struct is %llu - the snapshot is refused whole",
                        static_cast<unsigned long long>(dynamicBytes == nullptr ? 0
                                                                                : dynamicBytes->size()),
                        static_cast<unsigned long long>(sizeof(caps.Dynamic)));
                return false;
            }
            std::memcpy(&caps.Dynamic, dynamicBytes->data(), sizeof(caps.Dynamic));
            caps.CallMask = snapshot->callMask();

            RendererInfo renderer{};
            const auto* rendererBytes = snapshot->rendererInfo();
            if (rendererBytes == nullptr ||
                !DecodeRendererInfo(rendererBytes->data(), rendererBytes->size(), renderer)) {
                MGLOG_E("MG_Remote client: CapsSnapshot's rendererInfo blob did not decode");
                return false;
            }

            MG_Backend::FormatCapabilityCache formats{};
            const auto* formatBytes = snapshot->formatCaps();
            if (formatBytes == nullptr ||
                !DecodeFormatCapabilities(formatBytes->data(), formatBytes->size(), formats)) {
                MGLOG_E("MG_Remote client: CapsSnapshot's formatCaps blob did not decode");
                return false;
            }

            const String apiVersion =
                snapshot->apiVersion() == nullptr ? String{} : String{snapshot->apiVersion()->c_str()};

            // THE SERVER'S BACKEND TYPE, NEVER A NEW "Remote" ENUMERATOR and never guessed from
            // the renderer string: GL_Framebuffer.cpp:47, GL_Texture.cpp:6536 and
            // CompileEnv.cpp:122 SWITCH on it, and a value they do not know takes a wrong arm
            // rather than failing.
            const Uint32 rawBackend = snapshot->backendType();
            if (rawBackend >= static_cast<Uint32>(BackendType::BackendTypeCount)) {
                MGLOG_E("MG_Remote client: CapsSnapshot names backend type %u, which this build "
                        "has no enumerator for - the snapshot is refused rather than folded onto "
                        "Unknown, which three frontend switches would silently mis-branch on",
                        static_cast<unsigned>(rawBackend));
                return false;
            }
            CapsMirrorInstance().Adopt(caps, formats, renderer, apiVersion,
                                       static_cast<BackendType>(rawBackend));
            return true;
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
        std::unique_ptr<Transport::InProcessTransport> clientEnd;
        std::unique_ptr<Transport::InProcessTransport> serverEnd;
        Transport::InProcessTransport::CreatePair(clientEnd, serverEnd);
        return StartOverTransportPair(std::move(clientEnd), std::move(serverEnd));
    }

    MobileGLResult ClientSession::StartOverTransportPair(
        std::unique_ptr<Transport::InProcessTransport> clientEnd,
        std::unique_ptr<Transport::InProcessTransport> serverEnd) {
        if (m_started) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (clientEnd == nullptr || serverEnd == nullptr) {
            MGLOG_E("MG_Remote client: StartOverTransportPair needs both ends of one "
                    "InProcessTransport::CreatePair");
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_clientTransport = std::move(clientEnd);
        m_serverTransport = std::move(serverEnd);
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
            // msg_as_Welcome() IS PART OF THE GUARD - flatbuffers' Verifier::VerifyTable is
            // `return !table || table->Verify(*this)`, so a NULL union member verifies while
            // msg_type() still reports Welcome. See ServerSession::Accept for the same guard.
            if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::Welcome ||
                envelope->msg_as_Welcome() == nullptr) {
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
        // NO STAGE RING. SEG_STAGE is package w1's encoder-local LINEAR ALLOCATOR:
        // a staged byte run carries no RingRecordHeader, nothing consumes SEG_STAGE,
        // and the allocator reclaims on retiredSeq. A RingProducer over
        // RingCursorSet::Stage would publish stageHead with nothing advancing the
        // two tails, so FreeBytes() would fall to zero the first time the head
        // lapped the capacity and never recover - a guaranteed hang. See
        // RingControl's stage triple in Ring.h.
        if (!m_cmd.Valid()) {
            Stop();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // PeerDoorbell() is the bell the SERVER parks on and this side rings; SelfDoorbell() is
        // this side's own. Which is which is the session's knowledge, not the transport's.
        m_producer.Attach(control, &m_cmd, &m_clientTransport->PeerDoorbell(),
                          &m_clientTransport->SelfDoorbell(), SpinUsFromConfig());

        m_replies = Transport::ReplySlotPool(m_shm.ReplyBase(), m_shm.ReplyBytes(),
                                             m_shm.ReplySlotCount());
        m_events = Transport::EventRingConsumer(m_shm.EventControl(), control,
                                                m_shm.EventRingBase(), m_shm.EventRingCapacity(),
                                                m_shm.EventSegmentBase());
        if (!m_replies.Valid() || !m_events.Valid()) {
            Stop();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        m_barrierArmed = VerbBarrierFromConfig();
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
                           Wire::SegmentView{m_shm.StageBase(), m_shm.StageBytes()});
        m_segments.Install(Wire::kSegReply,
                           Wire::SegmentView{m_shm.ReplyBase(), m_shm.ReplyBytes()});
        m_segments.Install(Wire::kSegEvent,
                           Wire::SegmentView{m_shm.EventSegmentBase(),
                                             m_shm.AnnouncedSize(Transport::SessionSegmentSlot::Event)});
        // nullptr for the stage producer, and that is the honest value: c0's
        // signature predates w1's ruling that SEG_STAGE is a linear allocator, and
        // the encoder reaches its bytes through the SegmentTable above. Handing it
        // a live RingProducer over a cursor triple nobody consumes would be the
        // half-wired shape this session exists not to have.
        m_encoder = Wire::PipeWireEncoder(control, &m_cmd, nullptr, &m_segments);
        m_encoder.SetStageRetirementDoorbell(m_producer.SelfDoorbell());

        // ---- 7. the first CapsSnapshot, if the server had a backend to publish one from.
        // ONE DRAIN, ONE ADOPTER (c1): PumpControlPlane below is the only thing in the client
        // that turns a CapsSnapshot into a CapsMirror generation, so R-12's "a second arrival
        // IS the invalidation" lives in exactly one place. s1's step here used to stop at
        // "the snapshot arrived and is verifiable"; it now goes all the way, through the same
        // function every later arrival goes through.
        if (PumpControlPlane() == 0) {
            MGLOG_W("MG_Remote client: the handshake carried no CapsSnapshot - the caps mirror is "
                    "a PLACEHOLDER until one arrives. Every caps read until then answers a "
                    "default and says so");
        }

        // BOTH, AND m_started FIRST. `Active()` is what the integration lane's skip reads and
        // `m_started` is what `EmitAndWait` reads, and c1's round-1 rewrite of this function
        // set only the second of the two - so a fully-handshaken session with an apply thread
        // running and a caps mirror adopted took Fatal{NoClientSession, "Clear"} on its very
        // first verb, which is the most confusing possible spelling of "the session is up".
        // The order matters for the same reason it does at the other end: `Active()` hands a
        // caller a session it may immediately emit on, so the flag that permits emitting has
        // to be true before the pointer that grants access to it is published. `Stop()` takes
        // them down in the mirror order (m_started = false, then g_active = nullptr).
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

        // ---- 9. AND ONLY NOW THE THIRTY-SEVEN WIRE EMITTERS (R-17). This is the line that
        // arms `integration-split`: the 21 `DirectGLES.Split.*` entries skip on
        // `ClientSession::Active() == nullptr`, and every one of the four arming facts is true
        // at exactly this point and at no earlier one.
        //
        // IT IS LAST, AND EACH OF THE FOUR REASONS IS A DIFFERENT FAILURE:
        //   - after Hello/Welcome (step 2-4), or an emitter would publish into a ring the peer
        //     has not mapped;
        //   - after the first CapsSnapshot (step 7), because R-8's liveness gates read the caps
        //     mirror and a PLACEHOLDER mirror consumes nothing - a record emitted before it
        //     would go to a server this client has not been told consumes that family;
        //   - after ServerLoop::Start (step 8), because EmitAndWait BLOCKS on appliedSeq and
        //     with no apply thread nothing advances it: the first resource_create would spend
        //     30 seconds in the barrier and then Fatal{BarrierTimeout};
        //   - on THIS thread, the one that called MG_Backend::Init(), because it is the GL
        //     thread and table 3 makes gPipeInputs its to touch while the barrier holds.
        // The publication is safe without a fence because the apply thread never reads these
        // tables - the server decodes straight into MGPipeApply* - and this thread wrote them
        // before it can reach any GL entry point.
        InstallClientWireTables();
        return MOBILEGL_OK;
    }

    void ClientSession::Stop() {
        // FIRST, BEFORE ANYTHING ELSE GOES AWAY (R-17 / codex 4). Every later step here frees
        // something an emitter dereferences - the rings, the segments, the transports - so a
        // routed GL-thread call that arrives during teardown must not run the applier on the
        // caller and must not reach a half-freed ring. Round 2 reinstalled the monolith adapters
        // HERE, which is running the applier on the caller - the forbidden path table 3 draws.
        // Uninstall now RAISES A FLAG and leaves the wire rows in place; the next routed call
        // aborts by name (Fatal{ClientTablesUninstalled}) inside RequireSession before it touches
        // anything. The monolith adapters go back only at the END of teardown
        // (ReinstallMonolithAfterTeardown), for the at-exit ~BufferObject deletes that reach a
        // process with no session at all - and by then the rings are gone, so the applier a
        // monolith adapter runs is a defined no-op rather than a use-after-free.
        UninstallClientWireTables();
        if (!m_started) {
            // Start's own failure paths land here with a half-built session. FIVE of them are
            // reached AFTER ServerSession::Accept has already returned OK, so tearing down
            // only the client half is not enough and gets three things wrong at once: the
            // server keeps its four mappings and stays m_accepted, so Accept's own guard
            // refuses every later Start and the process can never open a session again; the
            // process-wide segment resolver stays installed; and resetting m_serverTransport
            // destroys a transport that ServerSession::m_transport and its two Doorbell*
            // still point at. The server closes FIRST, in the same order the started path
            // gets right, and only then do the transports go.
            m_producer.Detach();
            m_shm.Close();
            Server::ServerSessionInstance().Close();
            m_clientTransport.reset();
            m_serverTransport.reset();
            m_transport = nullptr;
            // The rings are gone; put the monolith adapters back for a process that will make no
            // more routed calls except, possibly, at-exit deletes (codex 4).
            ReinstallMonolithAfterTeardown();
            return;
        }

        // TABLE 3's TEARDOWN ORDER, and every step of it is load-bearing.
        //
        // 1. publish and let the server drain. Bounded: a lost record must be a red lane, not
        //    a hung exit.
        Transport::RingControl* control = m_shm.CmdControl();
        if (control != nullptr) {
            // The PRODUCER's own last-published seq, not RingControl::submittedSeq. Ring.h:72-77
            // permits submittedSeq to be published lazily and Ring.h:243 encourages batching
            // the publish, so the shared watermark is allowed to lag the emitter - and a drain
            // that waited for `appliedSeq >= submittedSeq` would then under-wait and free an
            // emitter-owned var-tail while a record still names it. With the verb barrier armed
            // the two are equal; under MOBILEGL_IPC_VERB_BARRIER=0, R-1's negative control that
            // the phase has to run once, they are not.
            const Uint64 submitted = m_producer.LastPublishedSeq();
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
        LogWireLedger();
        m_producer.Detach();
        m_encoder = Wire::PipeWireEncoder();
        m_events = Transport::EventRingConsumer();
        m_replies = Transport::ReplySlotPool();
        m_cmd = Transport::RingProducer();
        m_shm.Close();
        Server::ServerSessionInstance().Close();
        m_clientTransport.reset();
        m_serverTransport.reset();
        m_transport = nullptr;
        m_started = false;
        if (g_active == this) {
            g_active = nullptr;
        }
        // AND ONLY NOW the monolith adapters go back (codex 4): every ring an emitter would have
        // used is freed above, so from here a routed call - an at-exit ~BufferObject delete - runs
        // the applier exactly as it does under monolith, which is the correct answer for a
        // process that no longer has a session. During the whole span above, the raised flag made
        // any routed call abort by name instead.
        ReinstallMonolithAfterTeardown();
    }

    Wire::PipeWireEncoder& ClientSession::Encoder() { return m_encoder; }

    CapsMirror& ClientSession::Caps() { return CapsMirrorInstance(); }

    // PACKAGE c1's. The barrier's wait and the reply's wait are ONE wait (R-3/R-5), which is
    // what makes a blocking ReadPixels, MapPersistent's decline and the four Bool acceptances
    // cost zero extra round trips - and the client may not re-derive any of those four answers
    // locally. s1 supplies the four primitives it composes from: Encoder(), Producer(),
    // WaitForApplied() and ReadReply().
    //
    // THE ORDER IS ENCODE -> PUBLISH+NOTIFY -> WAIT -> READ REPLY, and it is not negotiable.
    // Splitting the wait from the read is how a package ends up answering an acceptance
    // question locally, which is the c0f/c0g defect P4a paid two contract corrections for; and
    // "always accept" is ID-39's 66 lost DirectVulkan uploads with a wire in between.
    Uint64 ClientSession::EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                      const void* varTail, Uint64 varTailBytes, void* replyOut,
                                      Uint64 replyBytes, Int32* statusOut, Uint64* replySizeOut) {
        // One tail is the two-tail form with one entry (P5b d1). Nothing is duplicated: the
        // barrier policy below has exactly one body, and the encoder's one-tail EncodeRecord is
        // itself defined as the tails form with tailCount <= 1.
        const Wire::WireTail tail{varTail, varTailBytes};
        return EmitAndWaitTails(op, payload, payloadBytes, varTail != nullptr ? &tail : nullptr,
                                varTail != nullptr ? 1u : 0u, replyOut, replyBytes, statusOut,
                                replySizeOut);
    }

    Uint64 ClientSession::EmitAndWaitTails(MG_Pipe::MGPWireOp op, const void* payload,
                                           Uint64 payloadBytes, const Wire::WireTail* tails,
                                           Uint32 tailCount, void* replyOut, Uint64 replyBytes,
                                           Int32* statusOut, Uint64* replySizeOut) {
        Uint64 varTailBytes = 0;
        for (Uint32 i = 0; i < tailCount; ++i) varTailBytes += tails[i].Size;
        if (statusOut != nullptr) *statusOut = Wire::ReplySink::kStatusError;
        if (replySizeOut != nullptr) *replySizeOut = 0;
        if (!m_started) {
            MGLOG_F("MGPipe: Fatal{NoClientSession, \"%s\"} - EmitAndWait on a session that has "
                    "not started. There is no fall-through: a record that could not be emitted "
                    "is a verb that did not happen",
                    Wire::WireOpName(op));
            std::abort();
        }

        // R-1's INVARIANT, AS A RUNTIME CHECK RATHER THAN A SENTENCE. While the barrier holds,
        // at most one of {GL thread, apply thread} is runnable - and that is the whole reason a
        // single process-wide gPipeInputs is legal (table 3). The client is about to publish a
        // record whose fields the applier will read out of gPipeInputs, so the apply thread
        // being inside the applier right now means the invariant has already been broken and
        // the next record would be read against a half-written residual fill.
        if (ApplyThreadIsInsideApplier()) {
            MGLOG_F("MGPipe: Fatal{BarrierViolation, \"%s\"} - the apply thread is inside the "
                    "applier while the GL thread is emitting. R-1 makes at most one of them "
                    "runnable, which is what keeps one process-wide gPipeInputs legal",
                    Wire::WireOpName(op));
            std::abort();
        }

        const Uint64 seq = m_encoder.EncodeRecord(op, payload, payloadBytes, tails, tailCount);
        if (seq == Wire::kInvalidSeq) {
            // The ring refused it. NOT a silent drop and not a retry loop: R-10 says P5 does no
            // chunking and must prove it needs none, so a refusal is the proof failing.
            MGLOG_F("MGPipe: Fatal{RingOverrun, \"%s\"} - the command ring refused a %llu-byte "
                    "record. P5 does not chunk (R-10); this is the proof obligation failing, not "
                    "a back-pressure case",
                    Wire::WireOpName(op),
                    static_cast<unsigned long long>(payloadBytes + varTailBytes));
            std::abort();
        }

        // Publish the head, record submittedSeq, THEN ring - in that order, which is
        // SessionProducer's one job and RingTest.cpp:446's pin. Notify-then-publish loses the
        // wakeup.
        m_producer.PublishAndNotify(seq);

        // Does this row own a reply slot? kMGPipeCallFlags IS THE SINGLE SOURCE OF TRUTH
        // (R-16 / ID-31) - fourteen rows now, because the four Bool acceptance entry points
        // gained the flag. Asking the catalogue rather than the caller is what stops a caller
        // that forgot to pass a buffer from silently turning an answer into a guess.
        const Bool ownsReplySlot =
            (MG_Pipe::MGPipeCallFlagsFor(op) & static_cast<Uint32>(MG_Pipe::kReplySlot)) != 0;

        if (!m_barrierArmed && !ownsReplySlot) {
            // R-1's NEGATIVE CONTROL ARM, and the only thing it turns off is the barrier. A
            // reply-slot row still waits: the answer is not derivable here and R-5 forbids
            // inventing one, so MOBILEGL_IPC_VERB_BARRIER=0 makes the queue free-running, not
            // the client clairvoyant.
            return seq;
        }

        const BarrierWaitScope waiting;
        const Transport::SessionWait wait = m_producer.WaitForApplied(seq, kBarrierTimeoutMs);
        if (wait == Transport::SessionWait::ShutDown) {
            // The doorbell died: the server went away. The only thing that returns from a
            // kWaitForever park, and therefore the only way a client blocked in the barrier
            // survives a server that is gone. It is teardown, not a server fault - so the answer
            // handed back is DECLINED, not the ERROR the status was pre-set to (M4): a
            // reply-owning row that saw ERROR here would abort Fatal{ReplyError} on a shutting-
            // down session, which is what the "teardown legitimately reaches here" comment
            // promised would NOT happen. DECLINED is honest - "the verb did not happen" - and the
            // acceptance rows already treat it as `false` / nullptr without aborting.
            // ReadPixels intentionally refuses this with Fatal{ReadbackDeclined, "ReadPixels"}:
            // unlike acceptance rows it cannot return successfully without complete pixels.
            if (statusOut != nullptr) *statusOut = Wire::ReplySink::kStatusDeclined;
            MGLOG_E("MG_Remote client: the barrier for %s (seq %llu) woke on a dead doorbell; the "
                    "server is gone and this verb did not happen (reported as DECLINED, not ERROR)",
                    Wire::WireOpName(op), static_cast<unsigned long long>(seq));
            return seq;
        }
        if (wait != Transport::SessionWait::Reached) {
            MGLOG_F("MGPipe: Fatal{BarrierTimeout, \"%s\"} - appliedSeq did not reach %llu within "
                    "%u ms. A bounded wait is deliberate: a wedged CI job and a lost record look "
                    "identical from outside, and only one of them is a bug worth finding",
                    Wire::WireOpName(op), static_cast<unsigned long long>(seq), kBarrierTimeoutMs);
            std::abort();
        }

        // THE REVERSE CHANNEL IS DRAINED HERE, and here is the only place it can be: under the
        // barrier this is the one instant at which the apply thread is known not to be inside
        // the applier, and MGPipeClientOnGpuWritten / OnBufferWriteback write frontend objects.
        // It is what gives AwaitBufferWriteback something to have waited FOR: b1's third state
        // clears when the writeback lands, and the writeback lands on this ring.
        DrainEventRing(m_events);

        if (!ownsReplySlot) return seq;

        // THE SAME WAIT, NOT A SECOND ONE. appliedSeq >= seq already means the server wrote
        // this record's answer, because it writes the slot before it advances the watermark.
        Uint64 replySize = 0;
        Int32 status = Wire::ReplySink::kStatusError;
        if (!ReadReply(seq, replyOut, replyBytes, &status, &replySize)) {
            MGLOG_F("MGPipe: Fatal{ReplyMissing, \"%s\"} - seq %llu carries kReplySlot and the "
                    "server applied it, but its slot does not stamp that seq. The stamp is what "
                    "makes a wrong-slot read detectable rather than plausible (R-3)",
                    Wire::WireOpName(op), static_cast<unsigned long long>(seq));
            std::abort();
        }
        if (statusOut != nullptr) *statusOut = status;
        if (replySizeOut != nullptr) *replySizeOut = replySize;
        if (status == Wire::ReplySink::kStatusError) {
            MGLOG_E("MG_Remote client: %s (seq %llu) answered ERROR", Wire::WireOpName(op),
                    static_cast<unsigned long long>(seq));
        }
        // DECLINED is NOT an error and is deliberately not logged as one: it is how
        // MapPersistent says nullptr (R-6) and how the four Bool acceptance rows say false
        // (R-5). A client that treated it as a failure would re-create ID-39 from the other
        // side.
        if (replyOut != nullptr && replySize > replyBytes) {
            MGLOG_F("MGPipe: Fatal{ReplyTooLarge, \"%s\"} - the answer is %llu bytes and the "
                    "caller offered %llu. P5 does not chunk a reply",
                    Wire::WireOpName(op), static_cast<unsigned long long>(replySize),
                    static_cast<unsigned long long>(replyBytes));
            std::abort();
        }
        return seq;
    }

    Bool ClientSession::BarrierArmed() const { return m_barrierArmed; }

    // R-1's mutual-exclusion invariant, as two probes that answer honestly.
    //
    // THE CLIENT'S FLAG IS THREAD-LOCAL AND THE SERVER'S IS NOT, and the asymmetry is the
    // point: "am I inside a barrier wait" is a question about the calling thread, while "is the
    // apply thread inside the applier" is a question the GL thread asks about a DIFFERENT
    // thread - so the second has to be a shared atomic and the first must not be, or a second
    // GL thread would see the first one's wait as its own.
    Bool ClientSession::InBarrierWait() { return g_inBarrierWait; }
    Bool ClientSession::ApplyThreadIsInsideApplier() {
        return g_applyThreadInsideApplier.load(std::memory_order_acquire);
    }
    void ClientSession::NoteApplyThreadEnteredApplier() {
        g_applyThreadInsideApplier.store(true, std::memory_order_release);
    }
    void ClientSession::NoteApplyThreadLeftApplier() {
        g_applyThreadInsideApplier.store(false, std::memory_order_release);
    }

    Uint32 ClientSession::PumpControlPlane() {
        // NOT gated on m_started. The first snapshot arrives DURING Start(), before this session
        // is started or active - and s1's half-built teardown path depends on m_started staying
        // false until step 8 has succeeded, so the flag cannot be moved earlier to suit this.
        if (m_transport == nullptr) return 0;
        Uint32 adopted = 0;
        // Bounded rather than `while (true)`: a server that queued frames faster than this
        // drains them would otherwise hold the GL thread here for ever, and a frame backlog
        // deeper than this is a finding rather than a steady state.
        for (Uint32 guard = 0; guard < kMaxControlFramesPerPump; ++guard) {
            if (m_transport->PeekFrameSize() == 0) break;
            std::vector<Uint8> frame;
            if (ReceiveEnvelope(*m_transport, frame, 0) != MOBILEGL_OK) break;
            const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(frame);
            if (envelope == nullptr) {
                MGLOG_E("MG_Remote client: an unverifiable control frame (%llu bytes) was dropped",
                        static_cast<unsigned long long>(frame.size()));
                continue;
            }
            if (envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::CapsSnapshot) {
                // SurfaceOp / SurfaceReply / ResyncRequest / AuxRequest / LogLine are P6's and
                // P7's. Named rather than ignored, so a phase that starts sending one does not
                // discover this loop swallowing it.
                MGLOG_W("MG_Remote client: control message %d is not consumed in P5",
                        static_cast<int>(envelope->msg_type()));
                continue;
            }
            if (AdoptCapsSnapshot(envelope->msg_as_CapsSnapshot())) ++adopted;
        }
        return adopted;
    }

    Transport::SessionProducer& ClientSession::Producer() { return m_producer; }

    Transport::SessionWait ClientSession::WaitForApplied(Uint64 seq, Uint32 timeoutMs) {
        return m_producer.WaitForApplied(seq, timeoutMs);
    }

    Bool ClientSession::ReadReply(Uint64 seq, void* outBytes, Uint64 outCapacity, Int32* outStatus,
                                  Uint64* outSize) {
        return m_replies.Read(seq, outBytes, outCapacity, outStatus, outSize);
    }

    Uint32 ClientSession::MaxReplyBytes() const { return m_replies.MaxReplyBytes(); }

    Bool ClientSession::ReplyCanHold(Uint64 bytes) const { return m_replies.CanHold(bytes); }

    // ID-47. Forwarded verbatim so that the message, the boundary and the abort are the pool's
    // and are pinned once, in SessionTest, rather than re-derived per caller.
    void ClientSession::RequireReadPixelsReplyFits(Uint32 width, Uint32 height, Uint32 format,
                                                   Uint32 type, Uint64 bytes) const {
        m_replies.RequireReadPixelsFits(width, height, format, type, bytes);
    }

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

    // R-10's AND R-9's numbers IN EVERY SPLIT PRIVATE LOG, not only in the lanes that set
    // MOBILEGL_PIPE_STATS=1.
    //
    // WHY IT IS HERE AND NOT ONLY ON THE STATS LINE. `MGPipe stats:` is an opt-in channel: two
    // ctest entries out of 21 set MOBILEGL_PIPE_STATS, and neither the retrace lanes nor the
    // 19 ordinary split entries do. R-10's proof obligation is about THE PHASE, not about the
    // two counting lanes - "no record on the reduced path comes near half the ring" has to be
    // readable from any split run that happened, which is what ID-53's per-entry private log
    // is for. One line per session teardown costs nothing and cannot be missed.
    //
    // IT IS ALSO WHERE THE PROOF FAILS SOFTLY. A record ABOVE the cap already aborts on the
    // spot with Fatal{RingOverrun} (PipeWireCodec.cpp), so this line's job is the other half:
    // a maximum that is merely CLOSE to the cap is not a crash and would otherwise be
    // invisible until the day a workload crossed it. The percentage is printed for exactly
    // that reason, and R-10 names the integrator as the person who decides between early
    // chunking and a bigger default ring when it climbs.
    void ClientSession::LogWireLedger() const {
        const Uint64 maxRecord = m_encoder.MaxRecordBytesSeen();
        const Uint64 cap = m_encoder.MaxRecordBytesCap();
        // Integer permille rather than a float: this file has no <iomanip> and a "%.1f" of a
        // ratio nobody can reproduce by hand is worse than two integers.
        const Uint64 permille = cap != 0 ? (maxRecord * 1000ull) / cap : 0ull;
        MGLOG_I("MG_Remote client: wire ledger: maxrec=%llu maxrecop=%s cap=%llu (%llu.%llu%% of "
                "RingProducer::MaxRecordBytes, half of a %llu byte SEG_CMD) cmdbytes=%llu "
                "ringwraps=%llu ringpads=%llu "
                "ringwaits=%llu emitseq=%llu - R-10's proof obligation and R-9's producer "
                "readings, published from the session that produced them",
                static_cast<unsigned long long>(maxRecord), m_encoder.MaxRecordOpName(),
                static_cast<unsigned long long>(cap),
                static_cast<unsigned long long>(permille / 10),
                static_cast<unsigned long long>(permille % 10),
                static_cast<unsigned long long>(cap * 2),
                static_cast<unsigned long long>(m_encoder.CmdBytesWritten()),
                static_cast<unsigned long long>(m_encoder.CmdWraps()),
                static_cast<unsigned long long>(m_encoder.CmdWrapPads()),
                static_cast<unsigned long long>(m_encoder.StageReclaimWaits()),
                static_cast<unsigned long long>(m_encoder.EmitSeq()));
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Client
