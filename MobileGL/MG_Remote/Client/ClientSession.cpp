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
#include "../Protocol/SurfaceOpCodec.h"
#include "../Transport/FdPassing.h"
#include "../Transport/InProcessTransport.h"
#include "WireTables.h"

#include <MGGitHash.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Pipe/MGPipeCallbacks.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
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
        // cp (CONTRACT-P6 D5b): bounded, and its expiry is NOT fatal. A peer that
        // is gone is a different fact from a peer that is slow, and only the
        // doorbell death latch may say which.
        constexpr Uint32 kRemoteControlTimeoutMs = 5000;
        // The server has to create a backend before it binds, and a cold
        // software rasteriser is not fast. Bounded, and a NAMED refusal at the end.
        constexpr Uint32 kSpawnConnectTimeoutMs = 20000;
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
        // A split client cannot observe a server's process-local diagnostic. The
        // role-local value remains useful to the applier itself; ordering between
        // roles is carried by the ring watermarks, not by this instrumentation.
        thread_local Bool g_roleInsideApplier = false;

        struct BarrierWaitScope {
            BarrierWaitScope() { g_inBarrierWait = true; }
            ~BarrierWaitScope() { g_inBarrierWait = false; }
            BarrierWaitScope(const BarrierWaitScope&) = delete;
            BarrierWaitScope& operator=(const BarrierWaitScope&) = delete;
        };

        // A DEADLOCK DETECTOR, NOT A PERFORMANCE GATE. The barrier exists so that a record the
        // applier never retires is reported instead of hanging: a lost record never completes at
        // all, while a slow apply completes late. The bound only has to sit far above the slowest
        // legitimate apply.
        //
        // lavapipe/llvmpipe have legitimate apply-thread stalls that reach the tens of seconds:
        // texture-handle registration (vkCreateImageView -> lvp_CreateImageView ->
        // llvmpipe_register_texture) blocks the apply thread on a JIT/futex path, measured at
        // 29.3-39.7 s in a single stall, ~44.6 s cumulatively over one inproc retrace run. A 30 s
        // bound therefore red-lanes those runs intermittently while the same stall on a monolith
        // run is simply slow - the monolith has no equivalent cap on this path. 120 s is >3x the
        // largest observed stall and keeps the watchdog's job: a record that is truly lost still
        // terminates the run, and the Fatal names the seq.
        constexpr Uint32 kBarrierTimeoutMs = 120000;

        Uint64 AppliedWaitBudgetMs(MG_Pipe::MGPWireOp op, const void* payload) {
            if (op != MG_Pipe::MGPWireOp::FenceWait) return kBarrierTimeoutMs;
            const Uint64 timeoutNs = static_cast<const MG_Pipe::MGPFenceWait*>(payload)->TimeoutNs;
            // ClientWaitSync may legitimately block longer than the ordinary verb barrier.
            // Round up without overflowing UINT64_MAX, then allow the usual transport grace.
            // FenceWaitServer queues a GPU wait; its GL_TIMEOUT_IGNORED is not a CPU budget.
            return timeoutNs / 1000000 + (timeoutNs % 1000000 != 0) + kBarrierTimeoutMs;
        }

        // Defined below; declared here because the applied wait has to be able to drain
        // SEG_EVENT while it is parked (P5e §2.6).
        Uint32 DrainEventRing(Transport::EventRingConsumer& events);

        Transport::SessionWait WaitForAppliedBudget(Transport::SessionProducer& producer,
                                                    Transport::EventRingConsumer& events,
                                                    Uint64 seq, Uint64 remainingMs) {
            // The transport takes Uint32 milliseconds with UINT32_MAX meaning forever.
            // Keep even the largest GL timeout finite, using bounded chunks and preserving
            // the doorbell's immediate shutdown result. Avoid a giant chrono deadline too.
            constexpr Uint32 kMaxFiniteWaitMs = Transport::kWaitForever - 1;
            for (;;) {
                const Uint32 chunkMs = remainingMs > kMaxFiniteWaitMs
                                           ? kMaxFiniteWaitMs : static_cast<Uint32>(remainingMs);
                // P5e (ra, CONTRACT-P5E §2.6): THE PARK BREAKS ON TWO THINGS. A server that
                // ran out of SEG_EVENT stops producing and therefore stops applying, so a
                // waiter that could only be woken by appliedSeq would wait for a watermark
                // nothing is going to move - and it is holding the only drain there is. The
                // second exit is that case: drain, clear the latch, ring the server, park
                // again. `remainingMs` is deliberately NOT charged for it; a wake that did
                // the queue's work is not the caller's budget being spent.
                const auto wait = producer.WaitForAppliedOrEventBacklog(seq, chunkMs);
                if (wait == Transport::SessionWait::Reached) {
                    Transport::RingControl* control = producer.Control();
                    if (control == nullptr ||
                        Transport::Watermark::Reached(control->Progress.appliedSeq, seq)) {
                        return wait;
                    }
                    DrainEventRing(events);
                    continue;
                }
                if (wait != Transport::SessionWait::TimedOut || remainingMs <= chunkMs) return wait;
                remainingMs -= chunkMs;
            }
        }
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
        //
        // THE CONSUMERS ARE CALLED BY NAME, NEVER THROUGH gMGPipeCallbacks (CONTRACT-P5C
        // §4.1): with an active transport the global table's reverse entries are the SERVER
        // session's producer callbacks, so routing the drain through it would call the
        // producer back from the GL thread - the layer-2 violation the contract names.
        //
        // THE SURFACE-CHANGED CONSUMER IS WHERE R3's OWNERSHIP ALWAYS WAS: the backend
        // posts the MGPSurfaceInfo, and the allocate/format writes against
        // pDefaultFramebufferInfo happen HERE, on the GL thread, against client memory.
        void ApplySurfaceChangedToClient(const MG_Pipe::MGPSurfaceInfo& info) {
            auto& defaultFBOInfo = MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo;
            if (!defaultFBOInfo) return;
            const auto format = static_cast<TextureInternalFormat>(info.InternalFormat);
            auto* colorTex =
                static_cast<MG_State::GLState::TextureObject2D*>(defaultFBOInfo->colorAttachment.get());
            auto* depthTex =
                static_cast<MG_State::GLState::TextureObject2D*>(defaultFBOInfo->depthAttachment.get());
            auto* stencilTex =
                static_cast<MG_State::GLState::TextureObject2D*>(defaultFBOInfo->stencilAttachment.get());
            if (info.Width != 0 && info.Height != 0) {
                // The SWAPCHAIN's publication shape: an extent-carrying event. Reproduces
                // SwapchainObject's monolith writes statement for statement - colour storage
                // at the extent, depth/stencil format then storage - because
                // FramebufferObject::CheckCompleteness requires every attachment to agree.
                const Int extentWidth = static_cast<Int>(info.Width);
                const Int extentHeight = static_cast<Int>(info.Height);
                const SizeT attachmentByteSize =
                    static_cast<SizeT>(info.Width) * static_cast<SizeT>(info.Height) * 4;
                if (colorTex != nullptr) {
                    colorTex->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                              {{extentWidth, extentHeight, 1}, attachmentByteSize});
                }
                if (depthTex != nullptr) {
                    depthTex->SetInternalFormat(format);
                    depthTex->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                              {{extentWidth, extentHeight, 1}, attachmentByteSize});
                }
                if (stencilTex != nullptr) {
                    stencilTex->SetInternalFormat(format);
                    stencilTex->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                                {{extentWidth, extentHeight, 1}, attachmentByteSize});
                }
            } else {
                // The DirectGLES publication shape: FORMAT ONLY, Width/Height == 0. The
                // placeholder's 512x512 extent is deliberately left alone - all three
                // attachments share it, and resizing depth/stencil without colour would
                // report the default framebuffer incomplete.
                if (depthTex != nullptr) depthTex->SetInternalFormat(format);
                if (stencilTex != nullptr) stencilTex->SetInternalFormat(format);
            }
        }

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
                    // The blobref names SEG_EVENT and the IN-SEGMENT offset of those inline
                    // bytes - never a host address (R-2's rule B), which is the whole reason
                    // EventRingConsumer exposes OffsetInSegment at all. The consumer's
                    // kSegEvent arm resolves it through this session's own SegmentTable.
                    MG_Pipe::MGPBlobRef blob{};
                    blob.Seg = Wire::kSegEvent;
                    blob.Offset = events.OffsetInSegment(bytes);
                    blob.Size = head->Size;
                    MG_Pipe::MGPipeClientOnBufferWriteback(
                        MG_Pipe::MGPipeHandle{head->Resource.Slot, head->Resource.Gen},
                        head->Offset, blob);
                    ++delivered;
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
                    // EventRange and MGPRange are the same two Uint64s (EventRing.h:61-66
                    // asserts it), so the tail is handed over as-is rather than copied into
                    // a second array a later reader could get out of step with.
                    const auto* ranges = reinterpret_cast<const MG_Pipe::MGPRange*>(
                        static_cast<const Uint8*>(view.payload) + sizeof(*head));
                    MG_Pipe::MGPipeClientOnGpuWritten(
                        MG_Pipe::MGPipeHandle{head->Resource.Slot, head->Resource.Gen},
                        static_cast<Uint>(head->RangeCount), ranges);
                    ++delivered;
                    break;
                }
                case Transport::kEventSurfaceChanged: {
                    if (view.payloadSize < sizeof(Transport::EventSurfaceChangedHead)) break;
                    const auto* head =
                        static_cast<const Transport::EventSurfaceChangedHead*>(view.payload);
                    MG_Pipe::MGPSurfaceInfo info{};
                    info.Width = head->Width;
                    info.Height = head->Height;
                    info.InternalFormat = head->InternalFormat;
                    info.Samples = head->Samples;
                    info.Layers = head->Layers;
                    info.IsDefault = head->IsDefault;
                    ApplySurfaceChangedToClient(info);
                    ++delivered;
                    break;
                }
                case Transport::kEventGlError: {
                    if (view.payloadSize < sizeof(Transport::EventGlErrorHead)) break;
                    const auto* head =
                        static_cast<const Transport::EventGlErrorHead*>(view.payload);
                    const char* message =
                        reinterpret_cast<const char*>(static_cast<const Uint8*>(view.payload) +
                                                      sizeof(*head));
                    const Uint64 tail = view.payloadSize - sizeof(*head);
                    // MessageBytes == 0 is the corrupt shape, never "no message": the NUL
                    // travels (CONTRACT-P5C §1, rule A's twin), so a legal record carries at
                    // least one byte and that byte terminates the string.
                    if (head->MessageBytes == 0 || tail < head->MessageBytes ||
                        message[head->MessageBytes - 1] != '\0') {
                        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"kEventGlError\"} - "
                                "MessageBytes %u against a %llu-byte tail, or the terminating "
                                "NUL is missing",
                                static_cast<unsigned>(head->MessageBytes),
                                static_cast<unsigned long long>(tail));
                        std::abort();
                    }
                    // The error queue is CLIENT state, written here on the GL thread. The
                    // observation point is the next drain after the post - P9 owns the
                    // ordering (CONTRACT-P5C §4.2).
                    if (MG_State::pGLContext != nullptr) {
                        MG_State::pGLContext->RecordError(
                            static_cast<ErrorCode>(head->Code),
                            MakeUnique<GenericErrorInfo>(
                                String(message, static_cast<SizeT>(head->MessageBytes - 1))));
                    } else {
                        MGLOG_E_ONCE("MG_Remote client: a kEventGlError (code %u) arrived with "
                                     "no live context and is dropped",
                                     static_cast<unsigned>(head->Code));
                    }
                    ++delivered;
                    break;
                }
                default:
                    MGLOG_W("MG_Remote client: reverse-channel record kind %u is not consumed in "
                            "P5c (four of the ten callbacks are armed; the rest are P9's)",
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

        return FinishStartup(&m_clientTransport->PeerDoorbell(),
                             &m_clientTransport->SelfDoorbell(),
                             /*startApplyThreadHere=*/true);
    }

    MobileGLResult ClientSession::StartSpawned() {
        if (m_started) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // A rendezvous nobody else can collide with. Independent processes need
        // a NAME; making it per-process-per-instant is what stops two runs on
        // one machine - which `ctest -j` produces by construction - finding each
        // other's server.
        // AN ABSTRACT NAME ('@'), not a path under /tmp. An Android app has no
        // writable /tmp and the first device run proved it the blunt way: the
        // server launched, bind() had nowhere to put the node, and the client
        // refused by name 20 s later. The abstract namespace needs no directory,
        // no mode bits and no cleanup after a crash, and its reach - the network
        // namespace - is exactly one app's processes.
        const std::string endpoint = std::string("@mgl-") +
                                     std::to_string(static_cast<long long>(::getpid())) + "-" +
                                     std::to_string(reinterpret_cast<std::uintptr_t>(this));

        const MobileGLResult launched =
            Server::LaunchServer(std::string(MG_Config::Ipc.ServerPath.c_str()), endpoint,
                                 &m_spawned);
        if (launched != MOBILEGL_OK) {
            return launched;
        }

        std::unique_ptr<Transport::SocketTransport> socket;
        // The bounded connect retry is what tolerates the server still binding -
        // and it is the CLIENT's, not the launcher's. An exhausted budget is a
        // NAMED refusal; there is no monolith fallback from here.
        const MobileGLResult connected =
            Transport::SocketTransport::ConnectTo(endpoint, kSpawnConnectTimeoutMs, socket);
        if (connected != MOBILEGL_OK) {
            int exitCode = 0;
            Server::ReapServer(m_spawned, 2000, &exitCode);
            MGLOG_E("MG_Remote client: could not reach the server we launched at \"%s\" "
                    "(rc=%d, server exit=%d)",
                    endpoint.c_str(), static_cast<int>(connected), exitCode);
            return connected;
        }
        // THE ONE FACT A MONOLITH OR inproc RUN CANNOT PRODUCE: a pid that is
        // not ours. The ConfigLoader marker proves the VALUE resolved; this
        // proves the SERVER ROLE LEFT THIS PROCESS, which is the whole claim the
        // SPAWN arm of the retrace matrix makes. run_trace_case.cmake requires
        // BOTH, so a spawn arm cannot go green on a transport that resolved and
        // then quietly ran the server role locally.
        const MobileGLResult started = StartOverSocket(std::move(socket));
        if (started == MOBILEGL_OK) {
            MGLOG_I("MG_Remote client: spawn ARMED - the server role runs in pid %d, reached at "
                    "\"%s\"",
                    m_spawned.pid, endpoint.c_str());
        }
        return started;
    }

    MobileGLResult ClientSession::StartOverSocket(
        std::unique_ptr<Transport::SocketTransport> transport) {
        if (m_started) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (transport == nullptr) {
            MGLOG_E("MG_Remote client: StartOverSocket needs a connected transport");
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_socketTransport = Move(transport);
        m_transport = m_socketTransport.get();

        // ---- Hello. The same bytes as inproc; the fingerprint is still compared
        // for equality, because under Dial == Fork the child IS the same binary and
        // comparing it catches a stale MOBILEGL_IPC_SERVER_PATH (CONTRACT-P6 4.1).
        const Uint64 fingerprint = CapsAbiFingerprint();
        {
            ::flatbuffers::FlatBufferBuilder builder(512);
            auto stamp = builder.CreateString(GIT_COMMIT_HASH_SHORT);
            auto hello = ::MobileGL::Wire::CreateHello(
                builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR, stamp,
                0u, 0u, 0, fingerprint);
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

        // ---- Welcome, and the four announced sizes. THERE IS NO LOCAL SERVER TO
        // ASK: the inproc path cross-checks Welcome against ServerSessionInstance()'s
        // own mapping, which a6 showed is vacuous against a real peer. Here the wire
        // values are the only values, which is what that check has to become on
        // every lane (CONTRACT-P6 4.4).
        Uint64 announced[4] = {0, 0, 0, 0};
        const Uint32 replySlotCount = Transport::kDefaultReplySlotCount;
        {
            std::vector<Uint8> frame;
            const MobileGLResult received =
                ReceiveEnvelope(*m_transport, frame, kHandshakeTimeoutMs);
            if (received != MOBILEGL_OK) {
                MGLOG_E("MG_Remote client: no Welcome from the spawned server within %u ms (rc=%d)",
                        kHandshakeTimeoutMs, static_cast<int>(received));
                Stop();
                return received;
            }
            const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(frame);
            if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::Welcome ||
                envelope->msg_as_Welcome() == nullptr) {
                MGLOG_E("MG_Remote client: the spawned server first frame is not a verifiable "
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
            const ::MobileGL::Wire::SegmentRef* refs[4] = {welcome->cmdRing(), welcome->stageRing(),
                                                           welcome->replyPool(),
                                                           welcome->eventRing()};
            for (int index = 0; index < 4; ++index) {
                if (refs[index] == nullptr || refs[index]->sizeBytes() == 0) {
                    MGLOG_E("MG_Remote client: Welcome SegmentRef %d is missing or zero-sized",
                            index);
                    Stop();
                    return MOBILEGL_ERR_PROTOCOL_MISMATCH;
                }
                announced[index] = refs[index]->sizeBytes();
            }
        }

        // ---- the four descriptors. THIS is the step inproc does not have, and the
        // reason SCM_RIGHTS is in this design at all: the segments are anonymous -
        // ASharedMemory_create has no filesystem name and memfd / shm_open+unlink
        // are not openable either - so the descriptor IS the only key.
        //
        // The slot travels in the sideband rather than being inferred from arrival
        // order, so a reordered or dropped offer is a NAMED mismatch instead of two
        // segments quietly swapped.
        // Seven offers: four segments, then THREE bell descriptors (slots 4..6).
        // Three because each side parks on one and rings another; see ServerMain.
        int fds[7] = {-1, -1, -1, -1, -1, -1, -1};
        {
            struct Sideband {
                Uint32 slot;
                Uint64 bytes;
            };
            const auto closeAll = [&fds]() {
                for (int& open : fds) {
                    if (open >= 0) {
                        ::close(open);
                        open = -1;
                    }
                }
            };
            for (int received = 0; received < 7; ++received) {
                Sideband sideband{};
                Uint64 sidebandSize = 0;
                int fd = -1;
                std::vector<Uint8> scratch(Transport::FdPassing::kMaxSidebandBytes);
                MobileGLMutableByteSpan span{scratch.data(), scratch.size()};
                const MobileGLResult got =
                    m_transport->ReceiveFd(&fd, span, &sidebandSize, kHandshakeTimeoutMs);
                if (got != MOBILEGL_OK || fd < 0) {
                    MGLOG_E("MG_Remote client: segment descriptor %d did not arrive (rc=%d)",
                            received, static_cast<int>(got));
                    closeAll();
                    Stop();
                    return got == MOBILEGL_OK ? MOBILEGL_ERR_PROTOCOL_MISMATCH : got;
                }
                if (sidebandSize != sizeof(Sideband)) {
                    MGLOG_E("MG_Remote client: segment offer %d carried %llu sideband bytes",
                            received, static_cast<unsigned long long>(sidebandSize));
                    ::close(fd);
                    closeAll();
                    Stop();
                    return MOBILEGL_ERR_PROTOCOL_MISMATCH;
                }
                std::memcpy(&sideband, scratch.data(), sizeof(Sideband));
                const bool isSegment = sideband.slot < 4;
                if (sideband.slot >= 7 || fds[sideband.slot] >= 0 ||
                    (isSegment && sideband.bytes != announced[sideband.slot])) {
                    MGLOG_E("MG_Remote client: segment offer names slot %u with %llu bytes; "
                            "Welcome announced a different size or the slot is already filled",
                            sideband.slot, static_cast<unsigned long long>(sideband.bytes));
                    ::close(fd);
                    closeAll();
                    Stop();
                    return MOBILEGL_ERR_PROTOCOL_MISMATCH;
                }
                fds[sideband.slot] = fd;
            }

            // ---- adopt and map. The same Adopt + Map every inproc run has
            // exercised since P5; only the origin of the descriptor is new.
            const MobileGLResult adopted = m_shm.AdoptFromDescriptors(
                fds, announced, replySlotCount, Transport::MemoryRole::Client);
            if (adopted != MOBILEGL_OK) {
                closeAll();
                Stop();
                return adopted;
            }
            // The four segment descriptors now belong to m_shm. The two bells
            // belong to the doorbells, which own and close them.
            //
            // WHICH BELL IS WHICH IS THE SESSION'S KNOWLEDGE, exactly as under
            // inproc: slot 4 is the pair end whose WRITE wakes the server, slot 5
            // is the one this side POLLS.
            // PARK ON ONE END, RING THE OTHER - the same split the server makes,
            // and for the same reason: a single-fd bell rings the PEER, so it
            // cannot wake its own waiter.
            //   fds[4] ring the SERVER      (ring-only, we never park on it)
            //   fds[5] our park end
            //   fds[6] ring OURSELVES
            m_socketPeerBell = MakeUnique<Transport::SocketDoorbell>(-1, fds[4], 1, true);
            m_socketSelfBell = MakeUnique<Transport::SocketDoorbell>(fds[5], fds[6], 1, true);
        }

        // `cp`: from here the twelve EGL forwarders cross the socket instead of
        // being posted into a mailbox this process does not own. Installed BEFORE
        // FinishStartup, because FinishStartup's caps pump can already trigger
        // one.
        Server::ServerLoopInstance().SetRemoteControlSink(&RemoteControlSinkThunk, this);

        // The apply thread IS ALREADY RUNNING, in the other process: ServerMain
        // started it before it sent the first descriptor. Starting one here would
        // put a second applier on a ring that already has one.
        return FinishStartup(m_socketPeerBell.get(), m_socketSelfBell.get(), false);
    }

    MobileGLResult ClientSession::RemoteControlSinkThunk(void* user,
                                                        Server::SurfaceControlFrame& frame) {
        return static_cast<ClientSession*>(user)->RunRemoteSurfaceControlFrame(frame);
    }

    MobileGLResult ClientSession::RunRemoteSurfaceControlFrame(Server::SurfaceControlFrame& frame) {
        if (m_transport == nullptr) {
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        // THE CLIENT MINTS, always, dense from 1 (CONTRACT-P6 §6.3). The server's
        // local mint exists for unnumbered LOCAL posts; a wire request that let
        // the server renumber it would get back a reply it cannot correlate.
        if (frame.seq == 0) {
            frame.seq = ++m_remoteControlSeq;
        }

        ::flatbuffers::FlatBufferBuilder builder(512);
        const MG_Remote::SurfaceWireError encoded = MG_Remote::EncodeSurfaceOpFrame(frame, &builder);
        if (encoded != MG_Remote::SurfaceWireError::None) {
            // A NAMED refusal, not a dropped op. InprocOnlyOpOnTheWire is the
            // live one: three of the twelve forwarders ride the frame channel
            // without a wire kind (CONTRACT-P6 §2), and a spawn client that
            // reaches one has found a gap rather than a transport error.
            MGLOG_E("MG_Remote client: surface op kind %d cannot cross the wire (%s)",
                    static_cast<int>(frame.kind), MG_Remote::SurfaceWireErrorName(encoded));
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        // ONE CONTROL OP AT A TIME. The twelve forwarders are already
        // caller-serialised on the server's m_callerMutex under inproc; the same
        // discipline has to hold here or two replies race for one correlation.
        const std::lock_guard<std::mutex> lock(m_remoteControlMutex);
        const MobileGLResult sent = m_transport->SendFrame(
            MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()});
        if (sent != MOBILEGL_OK) {
            return sent;
        }

        // The reply. A BOUNDED wait whose expiry is NOT fatal (CONTRACT-P6 §5.4
        // D5b): a peer that is gone is a different fact from a peer that is slow,
        // and only the doorbell's death latch may say which.
        for (;;) {
            std::vector<Uint8> reply;
            const MobileGLResult received =
                ReceiveEnvelope(*m_transport, reply, kRemoteControlTimeoutMs);
            if (received != MOBILEGL_OK) {
                MGLOG_E("MG_Remote client: no SurfaceReply for seq %llu within %u ms (rc=%d)",
                        static_cast<unsigned long long>(frame.seq), kRemoteControlTimeoutMs,
                        static_cast<int>(received));
                return received;
            }
            const ::MobileGL::Wire::CtrlEnvelope* envelope = ParseEnvelope(reply);
            if (envelope == nullptr) {
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            if (envelope->msg_type() == ::MobileGL::Wire::CtrlMsg::CapsSnapshot) {
                // The server republishes caps from inside its MakeCurrent
                // dispatch (a6 row A4-30), so a snapshot can arrive BEFORE the
                // reply to the very op that produced it. Adopting it here rather
                // than dropping it is what keeps R-12's "a second arrival IS the
                // invalidation" true on this path too.
                if (const auto* snapshot = envelope->msg_as_CapsSnapshot()) {
                    AdoptCapsSnapshot(snapshot);
                }
                continue;
            }
            if (envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::SurfaceReply ||
                envelope->msg_as_SurfaceReply() == nullptr) {
                MGLOG_E("MG_Remote client: expected a SurfaceReply, got message type %d",
                        static_cast<int>(envelope->msg_type()));
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            const ::MobileGL::Wire::SurfaceReply* wire = envelope->msg_as_SurfaceReply();
            if (wire->seq() != frame.seq) {
                MGLOG_E("MG_Remote client: SurfaceReply names seq %llu, we asked for %llu",
                        static_cast<unsigned long long>(wire->seq()),
                        static_cast<unsigned long long>(frame.seq));
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            MG_Remote::DecodeWireSurfaceReply(*wire, &frame);
            return MOBILEGL_OK;
        }
    }

    // The half of startup that is IDENTICAL for inproc and spawn: the ring
    // producer, the reply pool, the event ring, the segment table, the encoder,
    // the first CapsSnapshot and the emitter tables. Extracted by `sm` so the
    // spawn path cannot drift from the shape P5 spent five packages settling -
    // everything here is below the line where "which transport" stopped
    // mattering.
    //
    // `startApplyThreadHere` is the one policy difference, and it is not a
    // transport question: under inproc the client is what starts the server
    // role, because there is no server-side main to do it. Under spawn there
    // IS one, and it started the apply thread before it ever sent Welcome.
    MobileGLResult ClientSession::FinishStartup(Transport::Doorbell* peerBell,
                                                Transport::Doorbell* selfBell,
                                                bool startApplyThreadHere) {
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
        m_producer.Attach(control, &m_cmd, peerBell, selfBell, SpinUsFromConfig());

        m_replies = Transport::ReplySlotPool(m_shm.ReplyBase(), m_shm.ReplyBytes(),
                                             m_shm.ReplySlotCount());
        m_events = Transport::EventRingConsumer(m_shm.EventControl(), control,
                                                m_shm.EventRingBase(), m_shm.EventRingCapacity(),
                                                m_shm.EventSegmentBase());
        // P5e (ra, §2.6): the drain's other half. A server parked on a full SEG_EVENT is woken
        // by the drain that emptied it, not by the next thing this client happens to publish.
        m_events.SetServerDoorbell(peerBell);
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
        // P5e (ra, §2.6): the stage bell is a client wait and every client wait drains. The
        // encoder owns the park; the session owns SEG_EVENT, so the session lends it a drain.
        m_encoder.SetStageWaitHook(
            +[](void* self) { DrainEventRing(static_cast<ClientSession*>(self)->Events()); }, this);

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
        if (startApplyThreadHere) {
            Server::ServerSession& server = Server::ServerSessionInstance();
            const MobileGLResult running = Server::ServerLoopInstance().Start(server);
            if (running != MOBILEGL_OK) {
                Stop();
                return running;
            }
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
        //     the whole kBarrierTimeoutMs in the barrier and then Fatal{BarrierTimeout};
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
        //    kWaitForever (CondVarDoorbell::Kill): a Notify is consumed by one Park, after which
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
        // P5e (ra, §1): the LATCH GOES DOWN WITH THE SESSION. It is "what the first caps
        // adoption of THIS session said", and a session that starts again - the same singleton,
        // a second Start - must take it again from that session's own first snapshot rather
        // than inherit a verdict about a server that is gone. The present serial goes with it
        // for the same reason: the credit's id space belongs to the session that paces on it.
        m_runAheadArmed = false;
        m_runAheadLatched = false;
        m_presentsSent = 0;
        if (g_active == this) {
            g_active = nullptr;
        }
        // AND ONLY NOW the monolith adapters go back (codex 4): every ring an emitter would have
        // used is freed above, so from here a routed call - an at-exit ~BufferObject delete - runs
        // the applier exactly as it does under monolith, which is the correct answer for a
        // process that no longer has a session. During the whole span above, the raised flag made
        // any routed call abort by name instead.
        ReinstallMonolithAfterTeardown();
        // P6: the server we launched. Closing the transport is what it sees as
        // EOF and EOF is its whole exit condition, so the order is transport
        // first, reap second - and the reap is BOUNDED, because a server that
        // will not go is a different fact from one that is slow and only the
        // exit code can say which.
        if (m_spawned.pid >= 0) {
            if (m_socketTransport) {
                m_socketTransport->Shutdown();
            }
            int exitCode = 0;
            if (Server::ReapServer(m_spawned, 3000, &exitCode) != MOBILEGL_OK) {
                MGLOG_W("MG_Remote client: the spawned server (pid %d) did not exit within 3 s",
                        m_spawned.pid);
            } else if (exitCode != 0) {
                MGLOG_W("MG_Remote client: the spawned server exited %d", exitCode);
            }
        }
        Server::ServerLoopInstance().SetRemoteControlSink(nullptr, nullptr);

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
                                           Int32* statusOut, Uint64* replySizeOut,
                                           Bool wantReply) {
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

        // R-1's INVARIANT, AS A RUNTIME CHECK RATHER THAN A SENTENCE - but only while the
        // per-record barrier is the fencing model. With MOBILEGL_IPC_BATCH_WAITS=1 the
        // intended shape is exactly "the apply thread is inside the applier while the GL
        // thread emits the next value record", and the check would fire on the first batch:
        // its protection is already carried by the disjoint-field model (record-supplied
        // fields are written only by the applier from records and are never filled by the
        // client while the wire is live; BARRIER-PULLED fields are written only by the
        // client's residual fill and are read only at the pull-verbs, whose own wait makes
        // the fill quiescent during that apply). With the batch off, the original check
        // stands as written.
        if (MG_Config::Ipc.BatchWaits == 0 && ApplyThreadIsInsideApplier()) {
            MGLOG_F("MGPipe: Fatal{BarrierViolation, \"%s\"} - the apply thread is inside the "
                    "applier while the GL thread is emitting. R-1 makes at most one of them "
                    "runnable, which is what keeps one process-wide gPipeInputs legal",
                    Wire::WireOpName(op));
            std::abort();
        }

        Uint64 seq = m_encoder.EncodeRecord(op, payload, payloadBytes, tails, tailCount);
        if (seq == Wire::kInvalidSeq) {
            // EncodeRecord already refused individually oversized records. This one fits,
            // but appliedSeq may be ahead of retiredTail: wait for actual reclaimable space,
            // including the wrap pad. Prior EmitAndWait calls have published every record.
            Wire::WireRecordLayout layout{};
            Wire::MGPipeWireRecordLayout(op, payload, layout);
            const Uint64 toEnd = m_cmd.Capacity() - m_cmd.LocalHead() % m_cmd.Capacity();
            const Uint64 needed = layout.TotalBytes + (toEnd < layout.TotalBytes ? toEnd : 0);
            const BarrierWaitScope waiting;
            const auto wait = m_producer.WaitForCmdSpace(needed, kBarrierTimeoutMs);
            if (wait != Transport::SessionWait::Reached) {
                MGLOG_F("MGPipe: Fatal{RetirementWaitFailed, \"SEG_CMD\"} - %s needs %llu "
                        "reclaimable bytes; the retirement wait ended on %s",
                        Wire::WireOpName(op), static_cast<unsigned long long>(needed),
                        wait == Transport::SessionWait::ShutDown ? "shutdown" : "timeout");
                std::abort();
            }
            // P5e (ra, §2.6): EVERY WAIT EXIT DRAINS. The cmd-space wait is one of the two
            // safety nets under run-ahead (the stage bell is the other), and a client parked
            // here without draining is the other half of the flow-control deadlock: the server
            // stops applying on a full event ring, so retiredSeq stops moving, so this wait
            // never completes. One drain on the way out closes it.
            DrainEventRing(m_events);
            seq = m_encoder.EncodeRecord(op, payload, payloadBytes, tails, tailCount);
            if (seq == Wire::kInvalidSeq) {
                MGLOG_F("MGPipe: Fatal{RingOverrun, \"SEG_CMD\"} - %s refused after sufficient "
                        "space retired", Wire::WireOpName(op));
                std::abort();
            }
        }

        // Publish the head, record submittedSeq, THEN ring - in that order, which is
        // SessionProducer's one job and RingTest.cpp:446's pin. Notify-then-publish loses the
        // wakeup.
        m_producer.PublishAndNotify(seq);

        // Does this row own a reply slot? kMGPipeCallFlags IS THE SINGLE SOURCE OF TRUTH
        // (R-16 / ID-31) - fourteen rows now, because the four Bool acceptance entry points
        // gained the flag. Asking the catalogue rather than the caller is what stops a caller
        // that forgot to pass a buffer from silently turning an answer into a guess.
        //
        // P5e (ra) ADDS ONE CALLER-SIDE CONJUNCT AND NO SECOND SOURCE OF TRUTH: `wantReply`.
        // The catalogue still says which rows CARRY a slot; a caller that passes false says it
        // will not read this one (resource_subdata's buffer half, §2.5 - the only such caller).
        // The server posts either way, so the wire is unchanged and the row keeps its flag.
        const Bool rowCarriesReplySlot =
            (MG_Pipe::MGPipeCallFlagsFor(op) & static_cast<Uint32>(MG_Pipe::kReplySlot)) != 0;
        const Bool ownsReplySlot = rowCarriesReplySlot && wantReply;

        // ---- THE P5e WAIT RULE (CONTRACT-P5E §2.3) ---------------------------------------
        //
        // IT STANDS BEFORE THE BATCH SKIP AND REPLACES IT WHOLE, rather than adding a clause to
        // it. The batch skip is a rule about CALL CLASSES ("a set/CSO/object record carries its
        // whole value in the payload"); the wait class is a rule about WHAT AN APPLY READS, and
        // it is generated from the same PipeCalls.def row the class comes from
        // (MGPipeWaitClassFor, c0e). Two rules layered would mean a row could be skipped by the
        // class rule and waited by the column, and the column is the one the SERVER's
        // MGPipeBarriered reads - so the two halves of the wire would disagree about whether
        // the client is parked behind this record, which is the single fact §4.4's exemptions
        // and §3.3's detector both rest on.
        //
        // WITH RUN-AHEAD DISARMED NOTHING BELOW RUNS and the file's original shape continues -
        // Magma, a monolith transport, MOBILEGL_IPC_RUN_AHEAD=0 and every build before the
        // integration commit take the old path byte for byte, which is what makes RUN_AHEAD=0
        // a pure wait-rule A/B on identical server code (§5.8).
        if (RunAheadArmed() && !ownsReplySlot) {
            MG_Pipe::MGPipeWaitClass waitClass = MG_Pipe::MGPipeWaitClassFor(op);
            // §2.5's named relaxation, and the ONLY place a static column answer is overridden:
            // a kWaitReply row whose caller does not want the answer has nothing to wait FOR.
            // It is spelled here rather than in the column because the column is per-op and
            // this is per-call - the texture half of the same row still waits.
            if (!wantReply && waitClass == MG_Pipe::kWaitReply) waitClass = MG_Pipe::kWaitNone;
            switch (waitClass) {
            case MG_Pipe::kWaitNone:
                // Published and gone. Rule F is what makes this legal: the apply of an
                // unbarriered record names no client memory at all (§3, §4, §5).
                return seq;
            case MG_Pipe::kWaitPresent:
                // The credit wait already ran, in EmitPresent, BEFORE the encode (§2.4). There
                // is nothing left to do here and waiting would turn the credit into a barrier.
                return seq;
            case MG_Pipe::kWaitApplied:
                // §2.2's barriered rows: applier_reset, generate_mipmap (until tx2 lands its
                // VerbMipRes arm), set_storage_block_binding, the five XFB span verbs and the
                // CopyTex endpoint. Each still pulls a row or probes a registry, and the
                // client's wait is exactly what keeps P5C's semantics legal for it (ruling 4).
                // Falls through to the wait below.
                break;
            case MG_Pipe::kWaitReply:
            case MG_Pipe::kWaitClassCount:
                // Unreachable: `ownsReplySlot` is false here, and gen_pipe refuses a kWaitReply
                // row without a slot and a kReplySlot row that does not wait (c0e's three
                // negative controls). Treated as a wait rather than as a skip, because the
                // wrong answer to "does this hang" is the recoverable one.
                break;
            }
        }

        // MOBILEGL_IPC_BATCH_WAITS (default 1): a value-class record is published WITHOUT
        // waiting for its own apply. The barrier's one load-bearing reader is the backend
        // sync that pulls the object-class fields (CONTRACT-P5 table 2's remaining rows), and
        // those reads happen only at the kCtxVerb applies - draw / clear / blit / dispatch /
        // readback / XFB - plus the screen and query classes, and EVERY ONE OF THOSE STILL
        // WAITS below (as does every reply-slot row, whose answer the client consumes). So
        // the pulled fields are exactly as fresh at every read point as the per-record
        // barrier made them: between two waits the residual fill may move a field, but no
        // apply is reading it in between, and the next pull-verb's fill runs after the
        // previous pull-verb's apply was waited - the R-1 fence posts are untouched, only
        // the number of round trips between them changes. Set / CSO / object records carry
        // their whole value in the payload and are consumed at the next (waited) sync, which
        // the in-order ring guarantees precedes that sync's apply.
        //
        // WITH ONE NAMED EXCEPTION, FOUND BY ITS OWN FATAL (P5d round 3): generate_mipmap is
        // catalogued kCtxObject, but its sink is the backend's GenerateMipmap, which reads
        // MGB_CTX->GetActiveTextureUnit() and the unit's binding slot - residual inputs whose
        // freshness is the CLIENT's verb serial. Published without a wait, the record sits in
        // the ring while this thread runs the next verb's MGPipeValidateForVerb, which moves
        // that serial and withdraws the server stamp; the apply thread then reads the unit
        // stale and aborts Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@<next verb>"}
        // (F1WireScenario.GenerateMipmap*Pixels, reproduced on the gate tree once the round-3
        // spin no longer read the clock). So it waits, exactly as the four stamping verbs
        // do. The rule this encodes: a record may skip its wait only if its apply reads
        // nothing the residual fill writes - the value carried in the payload is the whole
        // input. No other kCtxObject row has a backend body behind it that reads MGB_CTX
        // (set_texture_params, resource_subdata, resource_readback and get_texture_image
        // own reply slots and wait anyway; the resource_* transfers and object_death resolve
        // by handle).
        //
        // AND IT IS DISARMED WHOLE UNDER RUN-AHEAD (P5e §2.3): the wait class above is the
        // rule there, and a second skip rule reading a different column is how the two halves
        // of the wire end up disagreeing about whether the client is parked. Written as a
        // conjunct here rather than left to "the classes happen not to overlap", because the
        // classes overlapping is one PipeCalls.def row away at any time.
        if (!ownsReplySlot && !RunAheadArmed() && MG_Config::Ipc.BatchWaits != 0 &&
            op != MG_Pipe::MGPWireOp::GenerateMipmap) {
            const MG_Pipe::MGPipeCallClass callClass = MG_Pipe::MGPipeCallClassFor(op);
            if (callClass == MG_Pipe::kCtxState || callClass == MG_Pipe::kCtxCso ||
                callClass == MG_Pipe::kCtxObject) {
                return seq;
            }
        }

        if (!m_barrierArmed && !ownsReplySlot) {
            // R-1's NEGATIVE CONTROL ARM, and the only thing it turns off is the barrier. A
            // reply-slot row still waits: the answer is not derivable here and R-5 forbids
            // inventing one, so MOBILEGL_IPC_VERB_BARRIER=0 makes the queue free-running, not
            // the client clairvoyant.
            return seq;
        }

        const BarrierWaitScope waiting;
        const Uint64 waitBudgetMs = AppliedWaitBudgetMs(op, payload);
        const Transport::SessionWait wait =
            WaitForAppliedBudget(m_producer, m_events, seq, waitBudgetMs);
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
                    "%llu ms. A bounded wait is deliberate: a wedged CI job and a lost record look "
                    "identical from outside, and only one of them is a bug worth finding",
                    Wire::WireOpName(op), static_cast<unsigned long long>(seq),
                    static_cast<unsigned long long>(waitBudgetMs));
            std::abort();
        }

        // THE REVERSE CHANNEL IS DRAINED HERE, and here is the only place it can be: under the
        // barrier this is the one instant at which the apply thread is known not to be inside
        // the applier, and MGPipeClientOnGpuWritten / OnBufferWriteback write frontend objects.
        // It is what gives AwaitBufferWriteback something to have waited FOR: b1's third state
        // clears when the writeback lands, and the writeback lands on this ring.
        DrainEventRing(m_events);

        if (!ownsReplySlot) {
            // P5e (ra, §2.3). A barriered record's apply has returned and this thread is the
            // only runnable one, so the four O-class SharedPtr rows the last fill left in
            // gPipeInputs can go. Under lockstep they were harmless - the next verb's fill
            // overwrote them - but under run-ahead the next fill may be a frame away, and a
            // frontend VAO or program whose LAST owner is that block is one the apply thread
            // can end up destroying (§2.7's deferred-destroy hazard). Released here rather
            // than at the fill, because here is where the applier is provably idle.
            if (RunAheadArmed()) ReleaseFillPins();
            return seq;
        }

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

    // ---- P5e (ra): the run-ahead latch and the waits it leaves behind ---------------------

    Bool ClientSession::RunAheadArmed() const { return m_runAheadArmed; }
    // THE PRODUCER'S NUMBER, NOT A SECOND ONE. SessionProducer records it inside
    // PublishAndNotify, which is the only place a record becomes published at all; a copy kept
    // here would be a second thing to forget to write on a path that publishes without waiting
    // - which, after P5e, is most of them.
    Uint64 ClientSession::LastPublishedSeq() const { return m_producer.LastPublishedSeq(); }
    Uint64 ClientSession::PresentCreditWaits() const { return m_presentCreditWaits; }

    void ClientSession::LatchRunAheadFromCaps() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // THE CONJUNCTION, IN THE CONTRACT'S OWN ORDER (§1). The caps read is last because it
        // is the only one that costs a mirror load, and because it is the only one whose answer
        // can change: the other two are process configuration.
        const Bool armed = m_barrierArmed && MG_Config::Ipc.RunAhead != 0 &&
                           Caps().HasCap(MG_Pipe::kCapRunAheadApply);
        if (!m_runAheadLatched) {
            m_runAheadLatched = true;
            m_runAheadArmed = armed;
            if (armed) {
                MGLOG_I("MG_Remote client: run-ahead ARMED - the server publishes "
                        "kCapRunAheadApply, so an unbarriered record is published and not "
                        "waited for (CONTRACT-P5E §2.3), paced by a present credit of %u",
                        MG_Config::Ipc.PresentCredit);
            } else if (MG_Config::Ipc.RunAhead != 0 && m_barrierArmed) {
                // THE KNOB IS NOT A SWITCH ON ITS OWN, and an operator who set it has to be
                // told that, or an A/B arm that measured nothing looks like an A/B arm that
                // measured no difference (Config.h's own argument for parsing it everywhere).
                MGLOG_W_ONCE("MG_Remote client: run-ahead requested, server does not publish "
                             "kCapRunAheadApply - running lockstep. The selected backend has "
                             "not enabled its implementation-readiness gate");
            }
            return;
        }
        // A LATER SNAPSHOT MAY ONLY TURN IT OFF. Promotion would mean the records already
        // published without a wait were published against a rule this server never agreed to,
        // and nothing can un-publish them; demotion only adds waits, so it is safe and is
        // exactly what a server that lost its run-ahead backend needs.
        if (!armed && m_runAheadArmed) {
            m_runAheadArmed = false;
            MGLOG_W("MG_Remote client: run-ahead DISARMED by a later caps snapshot - every "
                    "record from here waits as it did before P5e");
        }
#endif
    }

    void ClientSession::ReleaseFillPins() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // The block's own door: PipeInputs' O-class rows are private and MG_Remote has no
        // business knowing which four they are. MG_Impl owns the fill, so MG_Impl owns the
        // un-fill (PipeFill.cpp), and this side only says WHEN.
        MG_Pipe::MGPipeReleaseResidualFillPins();
#endif
    }

    void ClientSession::WaitForApplyToCatchUp(const char* why) {
#if MOBILEGL_BUILD_DISAGGREGATED
        // A NO-OP WITHOUT RUN-AHEAD, and that is not an optimisation: under lockstep the
        // client already waited for every record it published, so appliedSeq is at
        // LastPublishedSeq by construction and a second wait would only add a park on a
        // watermark that is already reached.
        if (!m_runAheadArmed || !m_started) return;
        const Uint64 target = m_producer.LastPublishedSeq();
        if (target == 0) return;
        const BarrierWaitScope waiting;
        const Transport::SessionWait wait =
            WaitForAppliedBudget(m_producer, m_events, target, kBarrierTimeoutMs);
        if (wait == Transport::SessionWait::ShutDown) {
            // The doorbell died: teardown, not a fault - the same answer EmitAndWaitTails
            // gives, and for the same reason.
            MGLOG_E("MG_Remote client: the forced wait for %s woke on a dead doorbell; the "
                    "server is gone", why);
            return;
        }
        if (wait != Transport::SessionWait::Reached) {
            MGLOG_F("MGPipe: Fatal{BarrierTimeout, \"%s\"} - a forced run-ahead wait did not "
                    "reach appliedSeq %llu within %u ms. The forced waits of CONTRACT-P5E §2.5 "
                    "are the points at which the client must be in step again, so a wedge here "
                    "is a wedge in the queue and not a slow frame",
                    why, static_cast<unsigned long long>(target), kBarrierTimeoutMs);
            std::abort();
        }
        // EVERY WAIT EXIT DRAINS (§2.6). It is what the flow-control deadlock argument rests
        // on: the server blocks only on a full event ring, the client blocks only on
        // watermarks the server advances, and every client wait drains - so at most one side
        // is parked at any instant.
        DrainEventRing(m_events);
        ReleaseFillPins();
#else
        (void)why;
#endif
    }

    void ClientSession::Finish() {
        // glFinish. Nothing goes on the wire - Flush and Finish have no record (ARCHITECTURE
        // §11) - and under lockstep there was nothing to do, because the client had already
        // waited out every command it issued. Under run-ahead "the commands issued so far have
        // completed" is a promise the queue can break, so this is where it is kept.
        WaitForApplyToCatchUp("glFinish");
    }

    Uint64 ClientSession::AcquirePresentCredit() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // MINTED CLIENT-SIDE AND 1-BASED (§1). Before P5e the client sent 0 and the server
        // stamped its own frame count, which was honest while nothing paced on it; a credit
        // needs an id space only the payer can advance, and that is this one.
        const Uint64 serial = m_presentsSent + 1;
        if (!m_runAheadArmed) {
            // Lockstep: the present record's own barrier IS the pacing, one frame deep by
            // construction. The serial is still minted so that both arms carry the same field
            // and the server's ReturnPresentCredit is fed on both - an A/B whose two arms
            // disagree about a wire field is not an A/B (Config.h's rule for the knobs).
            m_presentsSent = serial;
            return serial;
        }
        const Uint32 credit = MG_Config::Ipc.PresentCredit;
        if (m_presentsSent >= credit) {
            // Wait for swap number (serial - credit) to have come back. With credit 1 that is
            // the previous frame: the client publishes frame N+1's records while the server
            // applies and swaps frame N, and blocks here until swap N returned - one frame of
            // overlap and at most one frame of added latency (ruling 4 / ID-92).
            const Uint64 awaited = serial - credit;
            ++m_presentCreditWaits;
            const BarrierWaitScope waiting;
            Transport::SessionWait wait = Transport::SessionWait::TimedOut;
            for (;;) {
                // §2.6 again: the credit park breaks on a full SEG_EVENT too, for the same
                // reason the applied park does - a server that stopped producing stopped
                // swapping, and this waiter holds the only drain.
                wait = m_producer.WaitForPresentAckOrEventBacklog(awaited, kBarrierTimeoutMs);
                if (wait != Transport::SessionWait::Reached) break;
                Transport::RingControl* control = m_producer.Control();
                if (control == nullptr ||
                    Transport::Watermark::Reached(control->Progress.presentAckSerial, awaited)) {
                    break;
                }
                DrainEventRing(m_events);
            }
            if (wait == Transport::SessionWait::TimedOut) {
                MGLOG_F("MGPipe: Fatal{PresentCreditTimeout} - present %llu waited %u ms for "
                        "swap %llu to come back with a credit of %u. The credit, never the "
                        "ring's bytes, is what paces a run-ahead client (CONTRACT-P5E §2.4), "
                        "so a credit that never returns is a server that stopped swapping",
                        static_cast<unsigned long long>(serial), kBarrierTimeoutMs,
                        static_cast<unsigned long long>(awaited), credit);
                std::abort();
            }
            // ShutDown is teardown and returns: the present did not happen, the same answer
            // the barrier gives on a dead doorbell.
            if (wait == Transport::SessionWait::Reached) DrainEventRing(m_events);
        }
        m_presentsSent = serial;
        return serial;
#else
        return ++m_presentsSent;
#endif
    }

    // R-1's mutual-exclusion invariant, as two probes that answer honestly.
    //
    // THE CLIENT'S FLAG IS THREAD-LOCAL AND THE SERVER'S IS NOT, and the asymmetry is the
    // point: "am I inside a barrier wait" is a question about the calling thread, while "is the
    // apply thread inside the applier" is a question the GL thread asks about a DIFFERENT
    // thread in the shared-block control. Under role split each thread instead observes only
    // its own diagnostic; the client writes a different block and waits through appliedSeq.
    Bool ClientSession::InBarrierWait() { return g_inBarrierWait; }
    Bool ClientSession::ApplyThreadIsInsideApplier() {
        if (MG_Pipe::MGPipeRoleSplitActive()) return g_roleInsideApplier;
        return g_applyThreadInsideApplier.load(std::memory_order_acquire);
    }

    void ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt(const char* surface,
                                                                Bool isBarrieredFill) {
        // THE CHAIN IS A CONJUNCTION, SO ITS ORDER IS FREE - AND IT IS ORDERED CHEAPEST-FIRST
        // (P5d round 3, package D). Every line below is a pure predicate whose only effect is
        // "no violation, go home"; the Fatal fires on the AND of all of them, so reordering
        // cannot change which touches abort. It changes only what a touch that does NOT abort
        // pays, and this guard sits on the per-verb fill path (PipeFill.cpp:542, :1951, :2789
        // - MGPipeValidateForVerb is 4.11% self / 10.9% inclusive of the client thread), so
        // "what it costs to answer no" is the whole of its cost in a healthy process.
        //
        // The two config reads come first because they are the two answers that are constant
        // for the life of the process and true for nearly every process that runs this code:
        // a monolith build never has a role split at all, and MOBILEGL_IPC_BATCH_WAITS
        // defaults to 1, which retires this guard by design (see its note below). Only after
        // both miss do we pay for a role probe, two singleton reads, a shared atomic and the
        // thread_local at the end.
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        // ---- P5e (ra), CONTRACT-P5E §3.5: THE RUN-AHEAD RULE IS A DIFFERENT RULE -----------
        //
        // It is first, before the BatchWaits return, because BatchWaits' argument does not
        // survive run-ahead: "the pull-verbs' own wait keeps their pulled reads fenced" is a
        // statement about a client that waits at every verb, and this one does not. Under
        // run-ahead the block is SERVER-ROLE memory (§3) and the single legal GL-thread write
        // is the residual fill of a BARRIERED record - the one the client is about to park
        // behind. Every other touch races the applier by construction, so there is no
        // in-applier flag to consult and no "count it" arm: the value would be torn.
        //
        // This is where a fill left standing for an unbarriered record lands, by name.
        if (ClientSessionInstance().RunAheadArmed()) {
            // ---- P5e (ra2): THE EXEMPTION IS A FACT NOW, NOT A CLAIM ------------------------
            //
            // This arm used to read `if (isBarrieredFill) return;`, i.e. the caller's own
            // sentence "this touch is the residual fill of a record this thread is about to park
            // behind" was accepted as the whole argument. It is an argument about the FUTURE.
            // The order at the validate point is fill, then emit, then park, so at the instant
            // of the write the apply thread is still draining the unbarriered records the client
            // ran ahead of - and the measured consequence was the GL thread bumping
            // CurrentVerbSerial, withdrawing MGPipeServerClearVerbBoundary's flag and renaming
            // m_currentVerb underneath a record the applier was inside, which surfaced as
            // Fatal{UnmigratedPipeInput, "<field>@<the client's verb>"} on the apply thread.
            // Because the exemption was unconditional, this guard could not fire on any of it:
            // that is the answer to "does RefusePipeInputsTouchWhileApplierOwnsIt fire, and if
            // not, why not".
            //
            // So the sentence still exempts the fill, but only once it is TRUE of this instant.
            // The fill sites establish it by taking §2.5's forced wait first
            // (QuiesceApplierBeforeFill, PipeFill.cpp); removing that wait is the red-once and
            // lands here, by name, on the first barriered verb behind a run-ahead backlog.
            //
            // ApplyThreadIsInsideApplier() is an acquire load of the flag PipeApplier raises for
            // the whole of ApplyOne - stamp, decode and LeaveApplier included - so "not inside"
            // is exactly "no record is being applied" and not merely "not decoding".
            if (isBarrieredFill && !ApplyThreadIsInsideApplier()) return;
            if (Server::ServerLoop::OnApplyThread()) return; // the applier owns the block
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"gPipeInputs\"} - the GL thread touched "
                    "gPipeInputs (%s) on a RUN-AHEAD session %s. With the client running ahead "
                    "the block is the server's to read for as long as ANY record is in flight "
                    "(CONTRACT-P5E §3), so the only legal GL-thread write is a barriered fill "
                    "that has first taken §2.5's forced wait (QuiesceApplierBeforeFill)",
                    surface,
                    isBarrieredFill ? "in a barriered fill that did not first wait for the "
                                      "applier to catch up"
                                    : "outside a barriered fill");
            std::abort();
        }
        // MOBILEGL_IPC_BATCH_WAITS=1 makes "the fill runs while the apply thread applies an
        // earlier value record" the INTENDED shape: the fill writes only fields no record
        // supplies (the wire-live skips are the same answer the emitters use), the applier
        // writes only record-supplied fields, and the pull-verbs' own wait keeps their
        // pulled reads fenced. This guard exists for the model where that is not true, so
        // it stands only while the batch is off.
        if (MG_Config::Ipc.BatchWaits != 0) return;
        if (Server::ServerLoop::OnApplyThread()) return; // the applier owns the block inside a verb
        if (!ClientSessionInstance().Started()) return; // the bring-up window pre-dates the roles
        // MOBILEGL_IPC_VERB_BARRIER=0 is R-1's NEGATIVE CONTROL: the single-writer rule is off
        // by the operator's own hand there, and EmitAndWait's Fatal{BarrierViolation} owns the
        // red. Firing here instead would pre-empt the control's evidence line.
        if (!ClientSessionInstance().BarrierArmed()) return;
        if (!ApplyThreadIsInsideApplier()) return;
        // LAST, because it is the only thread_local left in the chain: an emutls call per
        // access in a shared library, and it must stay thread-local (see InBarrierWait's note
        // above - a second GL thread would otherwise read the first one's wait as its own).
        if (InBarrierWait()) return;
        MGLOG_F("MGPipe: Fatal{RoleViolation, \"gPipeInputs\"} - the GL thread touched gPipeInputs "
                "(%s) while the apply thread was inside the applier and this thread was not in a "
                "barrier wait. R-1's barrier is the only thing that makes one process-wide "
                "gPipeInputs legal (CONTRACT-P5 table 3); a touch in this window races the "
                "applier's own reads of it",
                surface);
        std::abort();
    }
    void ClientSession::NoteApplyThreadEnteredApplier() {
        g_roleInsideApplier = true;
        if (!MG_Pipe::MGPipeRoleSplitActive())
            g_applyThreadInsideApplier.store(true, std::memory_order_release);
    }
    void ClientSession::NoteApplyThreadLeftApplier() {
        g_roleInsideApplier = false;
        if (!MG_Pipe::MGPipeRoleSplitActive())
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
        // P5e (ra, §1): the run-ahead latch rides R-12's invalidation edge, because "the first
        // caps adoption after Start" and "the one place a CapsSnapshot becomes a mirror
        // generation" are the same instant, and a latch taken anywhere else would be reading a
        // mirror whose generation nobody had adopted yet. Called even when `adopted` is 0: the
        // first call in Start() is what takes the latch, and a pump that adopted nothing still
        // has to leave it taken (a handshake with no snapshot latches "not armed", which is
        // the honest answer for a server that never said what it consumes).
        if (adopted != 0 || !m_runAheadLatched) LatchRunAheadFromCaps();
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

    Uint32 ClientSession::DrainPublishedEvents() {
        // Not gated on m_started on purpose: PumpControlPlane's own pre-start call during
        // Start() has its ring-consumer twin here, and DrainEventRing's Valid() check is
        // the whole guard either case needs.
        return DrainEventRing(m_events);
    }

    Uint64 ClientSession::EventRingCapacityBytes() const { return m_shm.EventRingCapacity(); }

    Uint64 ClientSession::StageCapacityBytes() const { return m_shm.StageBytes(); }

    Wire::SegmentTable& ClientSession::Segments() { return m_segments; }

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
                "ringwaits=%llu emitseq=%llu cliwait=%llu clipark=%llu credit-waits=%llu - "
                "R-10's proof obligation, R-9's producer readings, P5d round 3's client wait "
                "pair and P5e's present credit, published from the session that produced them",
                static_cast<unsigned long long>(maxRecord), m_encoder.MaxRecordOpName(),
                static_cast<unsigned long long>(cap),
                static_cast<unsigned long long>(permille / 10),
                static_cast<unsigned long long>(permille % 10),
                static_cast<unsigned long long>(cap * 2),
                static_cast<unsigned long long>(m_encoder.CmdBytesWritten()),
                static_cast<unsigned long long>(m_encoder.CmdWraps()),
                static_cast<unsigned long long>(m_encoder.CmdWrapPads()),
                static_cast<unsigned long long>(m_encoder.StageReclaimWaits()),
                static_cast<unsigned long long>(m_encoder.EmitSeq()),
                // The same pair the summary line's wait[] carries, on the channel that exists
                // in EVERY split private log rather than in the two counting lanes: the round-3
                // profile runs did not set MOBILEGL_PIPE_STATS either, and "did the barrier spin
                // or did it park" is the question their logs have to be able to answer.
                static_cast<unsigned long long>(m_producer.Waits()),
                static_cast<unsigned long long>(m_producer.Parks()),
                // P5e (ra, §2.4). The number the device exit reads to say whether the present
                // credit is what paces this workload: 0 with credit 1 means the server is
                // never the frame's critical path, and a count near the frame count means it
                // always is. It is on this line rather than only on the stats line for
                // LogWireLedger's own reason - a trace replay never reaches a stats summary.
                static_cast<unsigned long long>(m_presentCreditWaits));
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Client
