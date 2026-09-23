// MobileGL - MobileGL/MG_Remote/Server/ServerSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package s1: the server half of a session.

#include "../Transport/SocketTransport.h"
#include "ServerSession.h"
#include <MG_Remote/FatalFunnel.h>

#include "../CapsCodec.h"
#include "../Handshake.h"
#include "../Transport/LinkMetrics.h"
#include "../Protocol/generated/protocol_generated.h"
#include "../Transport/InProcessTransport.h"

#include <Config.h>
#include <MGGitHash.h>
#include <MG_Pipe/MGPipeCallbacks.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// CONTRACT-P6 4.3: the one value in the handshake that a same-process session cannot fake.
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Server {

    namespace {
        // F1 (P7 wave 2). THE ONLY KNOB THAT CAN MAKE THE FIRST CapsSnapshot LATE.
        //
        // MOBILEGL_TEST_DELAY_FIRST_CAPS_MS delays the server's FIRST publication, and only
        // it - a re-send (R-12) is never delayed, because the question is about the window
        // before the client has any mask at all. Read once, on the first Accept, so a lane
        // that does not set it pays one getenv per session and nothing else.
        //
        // WHAT IT PROVES, AND WHERE. Under spawn / unix: / tcp:// the publication happens in
        // the SERVER's process, so the delay is a genuinely late snapshot from the client's
        // point of view and it exercises the client's step-7 wait (ClientSession.cpp:1284).
        // Under inproc there is no second process: Accept runs inside ClientSession::Start on
        // the client's own thread, so the delay simply makes Start slower - which is itself
        // the proof that under inproc the first snapshot CANNOT be late relative to session
        // start, and therefore that the only window in which the client could read a
        // placeholder is the one BEFORE Start is called. That window is F1's defect, and
        // MobileGL::Initialize closes it by ordering rather than by waiting.
        //
        // It is a TEST knob: it exists only in a disaggregated build and does nothing at all
        // unless it is set, so no production path can reach a delay it did not ask for.
        void DelayFirstCapsSnapshotForTest() {
#if MOBILEGL_BUILD_DISAGGREGATED
            static Bool spent = false;
            if (spent) return;
            spent = true;
            const char* text = std::getenv("MOBILEGL_TEST_DELAY_FIRST_CAPS_MS");
            if (text == nullptr || *text == '\0') return;
            const long ms = std::strtol(text, nullptr, 10);
            if (ms <= 0) return;
            MGLOG_W("MG_Remote server: MOBILEGL_TEST_DELAY_FIRST_CAPS_MS=%ld - the FIRST "
                    "CapsSnapshot is held back by that many milliseconds. This is F1's "
                    "test-only lever and must never be set in a measured run",
                    ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
        }

        // File-local on purpose: two callers, both handshake facts. A process-id helper on a
        // public header invites uses that are not.
        std::uint32_t SelfProcessId() {
#if defined(_WIN32)
            return static_cast<std::uint32_t>(::_getpid());
#else
            return static_cast<std::uint32_t>(::getpid());
#endif
        }
    } // namespace

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
            SessionFail(MGFatalFamily::UnsetCallMask, "MGPipe: Fatal{UnsetCallMask} - %s%s%s was never set on this ServerSession, "
                    "and there is no default: a guessed consumer mask makes the client's R-8 "
                    "liveness gates answer from a server-side fact the server never stated. The "
                    "client would stop emitting whole record families, clear its dirty flags on "
                    "acceptance anyway, and the lane would go green with the uploads lost "
                    "(ID-39, reflected). Call SetConsumedSubsystems() and SetCapabilityBits() "
                    "before Accept(); SetCapabilityBits(0) is a legitimate explicit answer",
                    capBitsSet ? "" : "SetCapabilityBits",
                    (!capBitsSet && !consumedSet) ? " and " : "",
                    consumedSet ? "" : "SetConsumedSubsystems");
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
            SessionFail(MGFatalFamily::EventRingOverflow, "MGPipe: Fatal{EventRingOverflow} - the producer of %s could not reserve "
                    "%llu bytes on SEG_EVENT. P5c's events are lossless and a full ring under "
                    "lockstep is a producer burst no measured workload has, so this is a "
                    "defect, not a drop (CONTRACT-P5C §4.4; the drop policy is P9's)",
                    eventName, static_cast<unsigned long long>(payloadBytes));
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
        // ONE NAMED REFUSAL SURVIVES, the impossible case: a single event larger than the ring
        // can EVER hold (no amount of draining helps). The two BUSY refusals that used to sit
        // beside it - a wait that ran out of patience, and two rounds against a ring the record
        // fits - are the PEER's behaviour and became the forfeit latch below (PH-6, ID-P7-2).
        // A server that does not publish kCapRunAheadApply keeps P5C's Fatal unchanged.
        Bool ServerPublishesRunAhead(const ServerSession& session) {
            if (!session.CallMaskIsSet()) return false;
            return (session.CallMask() & static_cast<Uint64>(MG_Pipe::kCapRunAheadApply)) != 0;
        }

        // PH-6 (ID-P7-2): MOBILEGL_IPC_EVENT_WAIT_MS (Config.h). It was a 30000 ms constant, and
        // it was spent TWICE per reservation (two rounds), so the plan's "60 seconds" was really
        // 30 s against a peer that never drains and a few milliseconds against one that trickles.
        Uint32 EventBacklogWaitMs() {
#if MOBILEGL_BUILD_DISAGGREGATED
            return MG_Config::Ipc.EventWaitMs;
#else
            return 2000;
#endif
        }

        Uint32 MillisecondsSince(std::chrono::steady_clock::time_point start) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            return elapsed <= 0 ? 0u : static_cast<Uint32>(elapsed);
        }

        void* ReserveEventOrBlock(ServerSession& session, Transport::EventKind kind,
                                  const char* eventName, Uint64 payloadBytes) {
            // FORFEITED: every event after the first drop is a drop too, at once - no Reserve and
            // no second wait. The session is on its way down, and the peer has already shown it
            // does not read this ring.
            if (session.ReverseChannelForfeited()) return session.DropEventAfterForfeit(payloadBytes);
            void* slot = session.Events().Reserve(kind, payloadBytes);
            if (slot != nullptr) return slot;
            if (!ServerPublishesRunAhead(session)) FatalEventRingOverflow(eventName, payloadBytes);

            // Impossible case one: the record does not fit an EMPTY ring. Reserve caps one
            // record at capacity/2 (Ring.h), so no drain can ever make room and blocking would
            // be a hang with a comment on it.
            const Uint64 cap = session.Events().Ring().MaxRecordBytes();
            if (payloadBytes + sizeof(Transport::RingRecordHeader) > cap) {
                SessionFail(MGFatalFamily::EventRingOverflow, "MGPipe: Fatal{EventRingOverflow} - %s needs %llu bytes and SEG_EVENT "
                        "caps ONE record at %llu (half its capacity). Flow control cannot help: "
                        "no amount of draining makes a record fit a ring that is too small for "
                        "it. SEG_EVENT's size is fixed (SessionSegmentSizes::EventRingBytes; the "
                        "MOBILEGL_IPC_EVENT_KB knob P5e proposed was never built), so slice the "
                        "event at its producer, as the writeback path already does",
                        eventName, static_cast<unsigned long long>(payloadBytes),
                        static_cast<unsigned long long>(cap));
            }

            const auto signals = session.DataLink()->Signals();
            Transport::Doorbell& bell = session.DataLink()->ConsumerBell();
            const auto drained = [signals] {
                return signals.EventRingFull->load(std::memory_order_acquire) == 0;
            };
            // THE WAIT ALSO ENDS ON A STOP (PH-6 fix round). ServerLoop::Stop() raises the session's
            // request and rings this same bell; a predicate that asked only "drained?" swallowed
            // that ring and parked again for the rest of the knob, so any knob over Stop()'s 5000 ms
            // join turned an ordinary end of the control stream into Fatal{ApplyThreadJoinTimeout}.
            const auto drainedOrStopping = [&drained, &session] {
                return drained() || session.ApplyStopRequested();
            };
            // ONE DEADLINE FOR THE WHOLE RESERVATION, NOT ONE PER PARK. The loop goes round as
            // often as the client clears the latch - that is how a client that drains in pieces
            // eventually makes room - but every round spends the same budget, so a peer that
            // clears the latch without freeing enough (it drains one slot per interval, or it
            // writes the flag itself) cannot hold the apply thread longer than the knob says.
            // The old shape gave up after TWO rounds, and called that "a corrupt cursor set":
            // a trickling peer reached it in milliseconds and took the process down with it.
            const Uint32 budgetMs = EventBacklogWaitMs();
            const auto start = std::chrono::steady_clock::now();
            const auto deadline = start + std::chrono::milliseconds(budgetMs);
            // Rounds in which the client DID clear the latch and the record still did not fit.
            // Zero at the deadline means the client never drained at all; more means it drained
            // too little, too slowly - two different peers, and the log says which one this was.
            Uint32 shortDrains = 0;
            const auto outOfPatience = [&]() {
                return shortDrains == 0
                           ? session.ForfeitReverseChannel("NotDraining", "the client did not drain SEG_EVENT",
                                                           eventName, payloadBytes, MillisecondsSince(start),
                                                           budgetMs, shortDrains)
                           : session.ForfeitReverseChannel("TooSlow",
                                                           "the client drained SEG_EVENT but never made room for "
                                                           "this record",
                                                           eventName, payloadBytes, MillisecondsSince(start),
                                                           budgetMs, shortDrains);
            };
            // Asked AFTER Dead() everywhere below: under spawn a stop usually FOLLOWS the peer's
            // hangup (control EOF), and the hangup is the more specific answer. The same fact can
            // reach the stop first from the other side - ServerMain notes an ended control stream
            // before it calls Stop() - and then it is still a hangup, not a stop.
            const auto stopped = [&]() {
                if (session.ControlStreamEnded()) {
                    return session.ForfeitReverseChannel(
                        "PeerGone", "the client's control stream ended while the server waited - the peer is gone",
                        eventName, payloadBytes, MillisecondsSince(start), budgetMs, shortDrains);
                }
                return session.ForfeitReverseChannel("Stopped",
                                                     "the session was stopped while the server waited for the "
                                                     "client to drain SEG_EVENT",
                                                     eventName, payloadBytes, MillisecondsSince(start), budgetMs,
                                                     shortDrains);
            };
            for (;;) {
                // PUBLISH AND RING FIRST. The client cannot drain a ring whose head it has not
                // been shown, and the latch Reserve just set is only useful to a client that
                // gets a bell with it.
                session.PublishEvents();
                // DEAD FIRST (the plan's own clause): a bell whose peer is gone answers every
                // Wait with an immediate false, and reporting that as "waited N ms" would name
                // a timeout for what is a hangup. Under spawn on the shm plane this needs the
                // bell's death witness (ServerMain.cpp, the control socket): the server holds
                // both ends of its own bell pair, so without one it can never go Dead().
                if (bell.Dead()) {
                    return session.ForfeitReverseChannel("PeerGone", "the client's doorbell is dead - the peer is gone",
                                                         eventName, payloadBytes, MillisecondsSince(start), budgetMs,
                                                         shortDrains);
                }
                if (!drained()) {
                    if (session.ApplyStopRequested()) return stopped();
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()).count();
                    if (left <= 0) return outOfPatience();
                    if (!bell.Wait(*signals.ConsumerParked, drainedOrStopping, /*spinUs=*/0,
                                   static_cast<Uint32>(left))) {
                        if (bell.Dead()) {
                            return session.ForfeitReverseChannel(
                                "PeerGone", "the client's doorbell died while the server waited - the peer is gone",
                                eventName, payloadBytes, MillisecondsSince(start), budgetMs, shortDrains);
                        }
                        return outOfPatience();
                    }
                    // Woken by Stop() and not by a drain. A drain that raced the stop still gets
                    // its Reserve below; the next round then sees the request and stops.
                    if (!drained()) {
                        if (bell.Dead()) {
                            return session.ForfeitReverseChannel(
                                "PeerGone", "the client's doorbell died while the server waited - the peer is gone",
                                eventName, payloadBytes, MillisecondsSince(start), budgetMs, shortDrains);
                        }
                        return stopped();
                    }
                }
                slot = session.Events().Reserve(kind, payloadBytes);
                if (slot != nullptr) return slot;
                // The client cleared the latch and the record STILL does not fit: it drained less
                // than this record needs. That is a slow drain, not a corrupt cursor set, and it
                // gets the rest of the same budget - Reserve has just raised the latch again, so
                // the next round parks until the client clears it once more.
                ++shortDrains;
                // A peer that clears the latch itself (it is on a page the peer can write) keeps
                // drained() true and this loop from parking; the budget still bounds it, and so
                // does a stop.
                if (session.ApplyStopRequested()) return stopped();
                if (std::chrono::steady_clock::now() >= deadline) return outOfPatience();
            }
        }

        // Accept owns the single process callback table. Its owner need not be the
        // convenience singleton; every reverse event must use the accepted instance's ring.
        ServerSession& ReverseCallbackOwner(const char* callback) {
            auto* session = ServerSession::Active();
            if (session == nullptr) {
                SessionFail(MGFatalFamily::RoleViolation, "MGPipe: Fatal{RoleViolation, \"%s.session-missing\"} - "
                        "a reverse callback has no accepted session owner", callback);
            }
            return *session;
        }

        // THE NUMBER THE BACKEND'S WRITEBACK PRODUCERS SLICE AGAINST (P3b/P4b espryt D1 slice 2).
        //
        // SEG_EVENT's capacity, published to MG_Pipe so that a producer in MG_Backend can ask it
        // without reaching either MG_Remote::Client (wrong linkage; that side answers the same
        // question for requests going the other way) or ServerSession (wrong layer). MG_Pipe turns
        // it into the per-record width - see MGPipeBufferWritebackSliceBytes - because the quarter
        // and its floor are one rule, not two. A session that has not attached a data link yet
        // answers 0, which reads as "do not slice" and is correct: with no ring there is nothing
        // to overflow.
        Uint64 ServerEventRingCapacityBytes() {
            auto* session = ServerSession::Active();
            if (session == nullptr) return 0;
            return session->Events().Ring().Capacity();
        }

        void ServerOnBufferWriteback(MG_Pipe::MGPipeHandle res, Uint64 offset,
                                     MG_Pipe::MGPBlobRef bytes) {
            ServerSession& session = ReverseCallbackOwner("OnBufferWriteback");
            if (bytes.Seg != MG_Pipe::kMGHostSpanSegNone) {
                // The backend handed over a segment-tagged blobref. The ONLY legal shape at
                // this boundary is the monolith one - Offset is the mapped address, valid
                // for this call - because the segment copy is THIS function's own job.
                SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"OnBufferWriteback.Seg\"} - the "
                        "writeback producer was handed Seg %u; at the backend boundary the "
                        "blobref names the backend's own mapped bytes (Seg = "
                        "kMGHostSpanSegNone) and the copy into SEG_EVENT is the producer's",
                        bytes.Seg);
            }
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
            ServerSession& session = ReverseCallbackOwner("OnGpuWritten");
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
            ServerSession& session = ReverseCallbackOwner("OnSurfaceChanged");
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
            ReverseCallbackOwner("OnGlError").PostGlError(code, message);
        }

        // One installer for both roles' tables, so the check exists in exactly one spelling:
        // writing over an entry somebody else claimed is Fatal{RoleViolation,
        // "callback-double-install"} - MGPipeCallbacks' "never over an entry a backend
        // already claimed" comment, made a check (CONTRACT-P5C §4.1). Finding OUR OWN
        // function there is the idempotent repeat, not a second installation.
        template <typename Fn>
        void InstallReverseCallback(Fn& entry, Fn producer) {
            if (entry != nullptr && entry != producer) {
                SessionFail(MGFatalFamily::RoleViolation, "MGPipe: Fatal{RoleViolation, \"callback-double-install\"} - a reverse "
                        "MGPipeCallbacks entry is already claimed by a different function. With "
                        "an active transport the four reverse entries are the server "
                        "session's producers; the client installs them under monolith only");
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

    ServerSession::ServerSession() {
        m_link = Transport::CreateSharedLink(Transport::TransportRoleTag::ServerConsumer);
        m_commands = &m_link->CommandsIn(); m_events = &m_link->EventsOut();
    }

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

    MobileGLResult ServerSession::Accept(Transport::ITransport& transport,
                                        const std::vector<Uint8>* firstFrame) {
        ServerSession* expectedOwner = nullptr;
        if (!g_sessionOwner.compare_exchange_strong(expectedOwner, this,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expectedOwner == this) return MOBILEGL_ERR_INVALID_ARGUMENT;
            SessionFail(MGFatalFamily::RoleViolation, "MGPipe: Fatal{RoleViolation, \"callback-double-install\"} - another "
                    "ServerSession already owns or is accepting the process reverse channel");
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
        // PH-6: a forfeit is THIS session's fact. A process that accepts again (inproc context
        // loss, a test's second session) starts with a reverse channel it has not given up.
        m_reverseChannelForfeit.store(false, std::memory_order_release);
        m_forfeitDrops.store(0, std::memory_order_release);
        // MOBILEGL_IPC_RING_MB / _STAGE_MB unless SetSegmentSizes overrode them. Unconditional
        // on purpose - see SizesFromConfig.
        if (!m_sizesSet) {
            m_sizes = SizesFromConfig();
        }

        // ---- 1/2. the ABI assertion, BEFORE a single record is decoded and before a byte of
        // shared memory exists. Its whole purpose is to refuse to interpret the peer's bytes.
        std::vector<Uint8> frame;
        if (firstFrame != nullptr) frame = *firstFrame;
        const MobileGLResult received = firstFrame != nullptr ? MOBILEGL_OK
            : ReceiveEnvelope(transport, frame, kHandshakeTimeoutMs);
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
            return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::MalformedHello,
                                   "first control frame is not a verifiable Hello");
        }
        const ::MobileGL::Wire::Hello* hello = envelope->msg_as_Hello();
        const char* theirStamp =
            hello->buildFingerprint() == nullptr ? nullptr : hello->buildFingerprint()->c_str();

        if (hello->dialMode() != ::MobileGL::Wire::DialMode::No &&
            hello->dialMode() != ::MobileGL::Wire::DialMode::Fork &&
            hello->dialMode() != ::MobileGL::Wire::DialMode::Connect)
            return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::MalformedHello,
                                   "unknown dial mode");
        const MobileGLResult compatible = ValidatePeerHandshake(transport, hello->abiMajor(),
            hello->abiMinor(), hello->wireFingerprint(), theirStamp, hello->dialMode());
        if (compatible != MOBILEGL_OK) return compatible;
        const Uint64 ourFingerprint = WireFingerprint();
        const auto* terms = hello->linkTerms();
        if (terms == nullptr || terms->wireForm() != ::MobileGL::Wire::WireForm::StructImage ||
            (terms->dataPlane() != ::MobileGL::Wire::DataPlane::SharedSegments &&
             terms->dataPlane() != ::MobileGL::Wire::DataPlane::Stream))
            return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::LinkTerms,
                                   "unsupported or missing link terms");
        const bool stream = terms->dataPlane() == ::MobileGL::Wire::DataPlane::Stream;
        if (stream && transport.Role() == Transport::TransportRole::InProcess)
            return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::LinkTerms,
                                   "stream requires a data connection");
        // PH-7 (1). One policy, one constant-time comparison, shared with ServerMain's supervisor
        // child (Handshake.h AuthenticatePeerToken). This site used std::strcmp, which returns at
        // the first differing byte.
        const MobileGLResult authenticated = AuthenticatePeerToken(transport, hello->token());
        if (authenticated != MOBILEGL_OK) return authenticated;
        if (hello->backendType() >= static_cast<Uint32>(BackendType::BackendTypeCount) ||
            (m_backend != nullptr && hello->backendType() != static_cast<Uint32>(m_backend->GetBackendType())))
            return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::Backend,
                "backend request differs from server", m_backend == nullptr ? 0 :
                static_cast<Uint32>(m_backend->GetBackendType()), hello->backendType());

        // ---- 3. the four segments, both control pages, the rings.
        MobileGLResult created = MOBILEGL_ERR_UNSUPPORTED;
        // PH-7 (4), ID-P7-3. Minted HERE - after the token was checked, before a byte of Welcome
        // exists - and bound after Welcome by BindDataConnection. The link is attached without
        // its descriptor: Welcome has to announce the sizes of memory the connection it names
        // will fill.
        Uint8 dataNonce[Transport::kDataNonceBytes] = {};
        Transport::ILink* pendingStream = nullptr;
        if (stream) {
            auto* socket = dynamic_cast<Transport::SocketTransport*>(&transport);
            if (socket == nullptr || !socket->IsTcp() || !m_dataSource)
                return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::LinkTerms,
                                       "stream needs a TCP control connection and a data-connection source");
            created = Transport::SocketTransport::MintNonce(dataNonce, sizeof(dataNonce));
            if (created != MOBILEGL_OK) {
                // (F fix round) The peer is told by name instead of seeing a bare close. The
                // return value stays the fault it is - exit 67 in ServerMain, counted in
                // sessionsFaulted - because a CSPRNG that gave nothing is this server's failure
                // and not the peer's terms; the code is LinkTerms because the terms asked for a
                // stream data plane and that is what could not be granted.
                (void)RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::LinkTerms,
                                      "stream data plane unavailable: the server could not mint a data nonce");
                return created;
            }
            std::unique_ptr<Transport::ILink> link;
            created = Transport::CreateDeferredStreamLink(m_sizes, Transport::TransportRoleTag::ServerConsumer, link);
            if (created == MOBILEGL_OK) {
                pendingStream = link.get();
                AttachDataLink(std::move(link));
            }
        } else {
            created = m_link->Memory().Create(m_sizes, Transport::MemoryRole::Server);
        }
        if (created != MOBILEGL_OK) {
            return created;
        }

        m_link->InitializeEndpoints();
        if (!m_commands->Valid()) {
            Close();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_link->BindDoorbells(&ConsumerDoorbell(), &ProducerDoorbell());
        m_consumer.Attach(*m_link, SpinUsFromConfig());
        m_replies = ReplyPool();
        m_replies.SetLink(m_link.get());


        m_consumer.SetLink(m_link->Capabilities().PublishIsDelivery ? nullptr : m_link.get());
        m_replies.SetLink(m_link.get());
        m_applier = PipeApplier(&m_segments, &m_replies);

        // ---- 4. Welcome: the four SegmentRefs, plus this side's half of the ABI statement.
        {
            ::flatbuffers::FlatBufferBuilder builder(1024);
            using Slot = Transport::SessionSegmentSlot;
            const auto segmentRef = [&](Uint32 id, ::MobileGL::Wire::SegmentKind kind, Slot slot) {
                return ::MobileGL::Wire::CreateSegmentRefDirect(builder, id, kind,
                                                                m_link->Memory().AnnouncedSize(slot),
                                                                m_link->Memory().AnnouncedName(slot));
            };
            auto cmd = segmentRef(1, ::MobileGL::Wire::SegmentKind::Cmd, Slot::Cmd);
            auto stage = segmentRef(2, ::MobileGL::Wire::SegmentKind::Stage, Slot::Stage);
            auto reply = segmentRef(3, ::MobileGL::Wire::SegmentKind::Reply, Slot::Reply);
            auto event = segmentRef(4, ::MobileGL::Wire::SegmentKind::Event, Slot::Event);
            auto stamp = builder.CreateString(BuildFingerprint());
            const auto nonce = pendingStream != nullptr
                ? builder.CreateVector(dataNonce, sizeof(dataNonce))
                : ::flatbuffers::Offset<::flatbuffers::Vector<Uint8>>();
            const Uint64 maxReply = m_sizes.ReplyBytes / m_sizes.ReplySlotCount - sizeof(Transport::ReplySlotHeader);
            const Uint64 cmdWindow = m_link->Memory().CmdRingCapacity();
            const Uint64 stageWindow = m_link->Memory().StageBytes();
            const Uint64 eventWindow = m_link->Memory().EventRingCapacity();
            // PH-8 (F fix round). The Hello's four counts are IGNORED, not clamped: nothing above
            // read them for sizing, and the session is the size this server chose. Said once when
            // a peer asked for more than it is granted, so an operator comparing a client's
            // configured windows with the server's sees which side sized the session (rule I: a
            // silent difference is the kind this phase removes). The production client asks for
            // nothing, so a session that was never mis-configured never logs this.
            if (terms->cmdWindowBytes() > cmdWindow || terms->stageWindowBytes() > stageWindow ||
                terms->eventWindowBytes() > eventWindow || terms->maxReplyBytes() > maxReply) {
                MGLOG_W("MG_Remote server: Hello asked for windows the server does not grant "
                        "(cmd=%llu stage=%llu event=%llu maxReply=%llu); the ask is ignored and the "
                        "session is server-sized (cmd=%llu stage=%llu event=%llu maxReply=%llu) - PH-8",
                        static_cast<unsigned long long>(terms->cmdWindowBytes()),
                        static_cast<unsigned long long>(terms->stageWindowBytes()),
                        static_cast<unsigned long long>(terms->eventWindowBytes()),
                        static_cast<unsigned long long>(terms->maxReplyBytes()),
                        static_cast<unsigned long long>(cmdWindow),
                        static_cast<unsigned long long>(stageWindow),
                        static_cast<unsigned long long>(eventWindow),
                        static_cast<unsigned long long>(maxReply));
            }
            auto negotiated = ::MobileGL::Wire::CreateLinkTerms(builder, terms->dataPlane(),
                ::MobileGL::Wire::WireForm::StructImage, maxReply, cmdWindow, stageWindow, eventWindow);
            auto welcome = ::MobileGL::Wire::CreateWelcome(
                builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                // CONTRACT-P6 4.3: THE SERVER'S OWN PID, not an echo of the client's.
                //
                // This field echoed hello->pid() - which is itself hard-coded 0 - so serverPid
                // was ALWAYS 0 and named nothing. §9.5's arm-proof gate wants the child pid and
                // this is its natural carrier: under spawn it is the one value in the handshake
                // that a same-process session cannot produce, because there the two pids are
                // equal by construction.
                static_cast<Uint32>(SelfProcessId()),
                cmd, stage, reply, event, stamp, ourFingerprint, ourFingerprint, negotiated,
                m_backend == nullptr ? hello->backendType() : static_cast<Uint32>(m_backend->GetBackendType()),
                nonce);
            auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
                builder, ::MobileGL::Wire::CtrlMsg::Welcome, welcome.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);
            const MobileGLResult sent = SendEnvelope(transport, builder);
            if (sent != MOBILEGL_OK) {
                Close();
                return sent;
            }
        }
        if (pendingStream != nullptr) {
            const MobileGLResult bound = BindDataConnection(transport, *pendingStream, dataNonce);
            if (bound != MOBILEGL_OK) {
                Close();
                return bound;
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
        m_segments.AttachLink(m_link.get());
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
        // ... and, beside them, the one NUMBER a writeback producer has to know: how much of
        // SEG_EVENT one record may be. Same ownership rule as the four entries above - installed
        // here, released at Close only if it is still ours - because a producer left holding a
        // dead session's ring width would slice against a ring that no longer exists.
        InstallReverseCallback(MG_Pipe::gMGPipeEventRingCapacityBytes, &ServerEventRingCapacityBytes);
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
            DelayFirstCapsSnapshotForTest();
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
        Uint64 callMask = CallMask();
        const auto timerSupported = m_backend->GetBackendFunctions().GL.IsTimerQuerySupported;
        if ((callMask & MG_Pipe::kCapTimerQuery) && (!timerSupported || !timerSupported()))
            callMask &= ~static_cast<Uint64>(MG_Pipe::kCapTimerQuery);

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

    // PH-7 (4), ID-P7-3. THE ONE PLACE A DATA CONNECTION IS BOUND TO A SESSION.
    //
    // Every connection the source offers is compared against this session's nonce, in constant
    // time. A match becomes the Stream data plane. A mismatch is refused BY NAME - to the log,
    // and to the peer on that same connection - and the wait goes on, so a stale connection
    // left over from an earlier session, or one raced in by somebody who can reach the port,
    // cannot end the session it failed to join. What ends the wait is the deadline or the
    // control peer going away; the deadline is refused on the control connection, where the
    // client is listening.
    MobileGLResult ServerSession::BindDataConnection(Transport::ITransport& control,
                                                     Transport::ILink& link, const Uint8* nonce) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeTimeoutMs);
        Uint32 refused = 0;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                MGLOG_E("MG_Remote server: no data connection presented this session's nonce within "
                        "%u ms (%u refused)", kHandshakeTimeoutMs, refused);
                return RefuseHandshake(control, ::MobileGL::Wire::RefuseCode::Authentication,
                                       "no data connection presented the session nonce");
            }
            int fd = -1;
            Uint8 presented[Transport::kDataNonceBytes] = {};
            const MobileGLResult offered = m_dataSource(static_cast<std::uint32_t>(left), &fd, presented);
            if (offered == MOBILEGL_ERR_TIMEOUT) continue;
            if (offered != MOBILEGL_OK) {
                MGLOG_E("MG_Remote server: waiting for the data connection ended (rc=%d, %u refused)",
                        static_cast<int>(offered), refused);
                return offered;
            }
            if (Transport::ConstantTimeNonceMatch(nonce, presented)) {
                const MobileGLResult bound = Transport::BindStreamLinkDataFd(link, fd);
#if !defined(_WIN32)
                if (bound != MOBILEGL_OK) ::close(fd);
#endif
                return bound;
            }
            ++refused;
            // The stray's transport owns `fd` and closes it on the way out of this scope.
            Transport::SocketTransport stray(fd, -1, Transport::TransportRole::Server);
            (void)RefuseHandshake(stray, ::MobileGL::Wire::RefuseCode::Authentication,
                                  "data connection nonce mismatch");
        }
    }

    void ServerSession::AttachDataLink(std::unique_ptr<Transport::ILink> link) {
        m_link = std::move(link);
        m_commands = &m_link->CommandsIn(); m_events = &m_link->EventsOut();
        m_consumer.SetLink(m_link->Capabilities().PublishIsDelivery ? nullptr : m_link.get());
        m_replies.SetLink(m_link.get());
        if (m_link) SetExternalDoorbells(&m_link->ConsumerBell(), &m_link->ProducerBell());
    }

    void ServerSession::FlushDataProgress() {
        if (m_link) m_link->FlushProgress();
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
            if (MG_Pipe::gMGPipeEventRingCapacityBytes == &ServerEventRingCapacityBytes) {
                MG_Pipe::gMGPipeEventRingCapacityBytes = nullptr;
            }
        }
        m_consumer.Detach();
        *m_commands = Transport::RingConsumer();
        *m_events = Transport::EventRingProducer();
        m_replies = ReplyPool();
        m_link = Transport::CreateSharedLink(Transport::TransportRoleTag::ServerConsumer);
        m_commands = &m_link->CommandsIn(); m_events = &m_link->EventsOut();
        m_externalConsumerBell = nullptr; m_externalProducerBell = nullptr;
        m_link->Memory().Close();
        m_transport = nullptr;
        m_accepted = false;
        ServerSession* expectedOwner = this;
        g_sessionOwner.compare_exchange_strong(expectedOwner, nullptr,
            std::memory_order_release, std::memory_order_relaxed);
    }

    Transport::RingConsumer& ServerSession::CommandRing() { return *m_commands; }

    Transport::RingControl& ServerSession::Control() {
        Transport::RingControl* control = m_link->Memory().CmdControl();
        if (control == nullptr) {
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ServerSession::Control\"} - the control "
                    "page does not exist until Accept() has mapped SEG_CMD");
        }
        return *control;
    }

    Wire::SegmentTable& ServerSession::Segments() { return m_segments; }
    PipeApplier& ServerSession::Applier() { return m_applier; }
    ReplyPool& ServerSession::Replies() { return m_replies; }

    // The bell the apply thread parks on. On the server endpoint of an InProcessTransport that
    // is SelfDoorbell(); the client reaches the same bell through its own PeerDoorbell().
    void ServerSession::SetExternalDoorbells(Transport::Doorbell* consumer,
                                             Transport::Doorbell* producer) {
        m_externalConsumerBell = consumer;
        m_externalProducerBell = producer;
    }

    Transport::Doorbell& ServerSession::ConsumerDoorbell() {
        // sm: injected first, because a spawn session's bells are sockets the
        // entry point made and the transport has never heard of.
        if (m_externalConsumerBell != nullptr) {
            return *m_externalConsumerBell;
        }
        if (m_transport == nullptr) {
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ServerSession::ConsumerDoorbell\"} - no "
                    "transport; Accept() has not run");
        }
        if (m_transport->Role() == Transport::TransportRole::InProcess) {
            return static_cast<Transport::InProcessTransport*>(m_transport)->SelfDoorbell();
        }
        // P6: SocketTransport's pair. The accessors stay off ITransport by ruling (contract
        // §3.9) precisely so that this stays one switch in one file rather than two virtuals
        // every transport has to invent a home for.
        SessionFail(MGFatalFamily::UnmigratedVerb, "MGPipe: Fatal{UnmigratedVerb, \"ServerSession::ConsumerDoorbell\"} - transport "
                "role %u has no doorbell pair yet; that is P6's SocketTransport",
                static_cast<unsigned>(m_transport->Role()));
    }

    Transport::Doorbell& ServerSession::ProducerDoorbell() {
        if (m_externalProducerBell != nullptr) {
            return *m_externalProducerBell;
        }
        if (m_transport == nullptr) {
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ServerSession::ProducerDoorbell\"} - no "
                    "transport; Accept() has not run");
        }
        if (m_transport->Role() == Transport::TransportRole::InProcess) {
            return static_cast<Transport::InProcessTransport*>(m_transport)->PeerDoorbell();
        }
        SessionFail(MGFatalFamily::UnmigratedVerb, "MGPipe: Fatal{UnmigratedVerb, \"ServerSession::ProducerDoorbell\"} - transport "
                "role %u has no doorbell pair yet; that is P6's SocketTransport",
                static_cast<unsigned>(m_transport->Role()));
    }

    Transport::SessionSegments& ServerSession::Shm() { return m_link->Memory(); }
    Transport::SessionConsumer& ServerSession::Consumer() { return m_consumer; }
    Transport::EventRingProducer& ServerSession::Events() { return *m_events; }
    Transport::ITransport* ServerSession::Control_Plane() { return m_transport; }

    void ServerSession::PublishEvents() {
        if (!m_accepted) {
            return;
        }
        // Publish, THEN ring - the same order as the forward direction, and the session picks
        // the bell so that no caller can pair the right ring with the wrong flag.
        m_events->Ring().Publish();
        m_consumer.NotifyClient();
        FlushDataProgress();
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

    // PH-6 (ID-P7-2). THE FIRST DROP, AND THE LATCH IT RAISES.
    //
    // Logged ONCE, by name, with everything a triage needs: which event, how big, what the
    // client did (never drained / drained too little / went away), how long the server waited
    // against which budget. MGLOG_E and not a Fatal marker on purpose: nothing here is this
    // process's defect, so neither the census nor a retrace's `Fatal{` scan may count it as one -
    // and the session that follows is an ordinary stop, exit 0, not a SessionFault.
    void* ServerSession::ForfeitReverseChannel(const char* cause, const char* why, const char* eventName,
                                               Uint64 payloadBytes, Uint32 waitedMs, Uint32 budgetMs,
                                               Uint32 shortDrains) {
        if (!m_reverseChannelForfeit.exchange(true, std::memory_order_acq_rel)) {
            MGLOG_E("MGPipe: ReverseChannelForfeit{%s} - %s needs %llu bytes on a full SEG_EVENT and %s "
                    "(waited %u ms of MOBILEGL_IPC_EVENT_WAIT_MS=%u, %u short drain(s)). The reverse "
                    "channel is lossless (CONTRACT-P5C §4.4), so a session that drops one event cannot "
                    "go on: this event and every later one is dropped and counted, and the session "
                    "stops by the ordinary stop path (PH-6, ID-P7-2)",
                    cause, eventName, static_cast<unsigned long long>(payloadBytes), why, waitedMs, budgetMs,
                    shortDrains);
        }
        return DropEventAfterForfeit(payloadBytes);
    }

    // The producer still fills and publishes what it was given, because its code is the same
    // whether the slot is SEG_EVENT or not; the scratch is where that write goes to die. Apply
    // thread only (SEG_EVENT has one producer), so the one buffer needs no lock.
    void* ServerSession::DropEventAfterForfeit(Uint64 payloadBytes) {
        if (m_events != nullptr) m_events->CountDrop();
        m_forfeitDrops.fetch_add(1, std::memory_order_acq_rel);
        const SizeT bytes = static_cast<SizeT>(payloadBytes == 0 ? 1 : payloadBytes);
        if (m_forfeitScratch.size() < bytes) m_forfeitScratch.resize(bytes);
        return m_forfeitScratch.data();
    }

    // Both of these advance AND ring, through SessionConsumer. The free functions in namespace
    // Watermark do not ring: a client parked in WaitForPresentAck(kWaitForever) needs the pair.
    void ServerSession::AdvanceCompletedFrame(Uint64 serial) {
        if (!m_accepted) {
            return;
        }
        m_consumer.CompleteFrame(serial);
        FlushDataProgress();
    }

    void ServerSession::ReturnPresentCredit(Uint64 serial) {
        if (!m_accepted) {
            return;
        }
        Transport::LinkMetricsServerPresent(serial);
        m_consumer.ReturnPresentCredit(serial);
        FlushDataProgress();
    }

    Transport::RoleMemorySample ServerSession::SampleMemory() const {
        return Transport::SampleRoleMemory(Transport::MemoryRole::Server);
    }

    void ServerSession::LogMemory(const char* phase) const {
        Transport::LogRoleMemory(phase, SampleMemory());
    }

} // namespace MobileGL::MG_Remote::Server
