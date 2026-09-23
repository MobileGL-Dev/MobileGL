// MobileGL - MobileGL/MG_Remote/Server/ServerSession.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server half of a session: the consumer side of the rings, the handshake reply, the caps
// snapshot. Owner: package s1. Signatures by c0.
//
// The four segment sizes are already pinned by ProtocolSmokeTest.cpp:72 and are not up for
// re-derivation here: SEG_CMD 8 MiB, SEG_STAGE 32 MiB, SEG_REPLY 8 MiB, SEG_EVENT 256 KiB.
// MOBILEGL_IPC_RING_MB and MOBILEGL_IPC_STAGE_MB move the first two; the ring caps ONE record
// at half its size (R-10). See SessionRings.h's header for why the ring inside SEG_CMD is 4 MiB
// rather than 8 - the control page takes the head and the capacity must be a power of two -
// and why that deviation from CONTRACT-P5 §5's arithmetic is in the safe direction.
//
// THE TWO DOORBELL ACCESSORS ARE ON THE CONCRETE CLASS, NOT ON ITransport
// (InProcessTransport.h:64-68). P5 decides this now rather than letting P6 discover it: the
// SESSION owns the pair and hands out references, so ITransport stays the dumb control-plane
// interface its header says it is and SocketTransport does not have to grow two accessors it
// has no natural home for. Discovering this in P6 would mean re-laying one package's call sites.
//
// WHO CREATES THE SEGMENTS: this side. Welcome announces all four SegmentRefs and Welcome is
// server -> client, so the server allocates and the client attaches. "Client-owned" in
// protocol.fbs's comments is about who WRITES a segment, not who allocates it.
//
// WHAT Accept() DOES, IN ORDER. The ABI assertion is FIRST, before a single record is decoded
// and before any segment exists, because its whole purpose is to refuse to interpret the peer's
// bytes at all:
//   1. receive Hello;
//   2. compare abiMajor/abiMinor and CapsAbiFingerprint() - Fatal{AbiMismatch} on a difference,
//      never a downgrade;
//   3. create and map the four segments, initialise both control pages, clear the reply pool;
//   4. send Welcome with the four SegmentRefs;
//   5. publish the first CapsSnapshot, IF a backend has been handed over (SetBackend). Without
//      one the session is accepted and the snapshot is deferred with a loud line: the caps
//      blob codecs are w1's and the server's private BackendObject is v1's, and a session that
//      refused to exist until both landed would block every other package's bring-up.

#pragma once
#include "../Transport/ILink.h"
#include <Includes.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include <MG_Pipe/MGPipe.h>

#include "../Transport/Doorbell.h"
#include "../Transport/EventRing.h"
#include "../Transport/ITransport.h"
#include "../Transport/ReplySlot.h"
#include "../Transport/Ring.h"
#include "../Transport/RoleMemory.h"
#include "../Transport/SessionRings.h"
#include "../Wire/PipeWireCodec.h"
#include "PipeApplier.h"

namespace MobileGL::MG_Remote::Server {

    class ServerSession {
    public:
        static ServerSession* Active();

        ServerSession();
        ~ServerSession();
        void AttachDataLink(std::unique_ptr<Transport::ILink> link);
        Transport::ILink* DataLink() const { return m_link.get(); }
        void FlushDataProgress();

        // Maps the four segments, answers Hello with Welcome, and publishes the first
        // CapsSnapshot. The ABI assertion (CapsCodec.h) happens HERE, before a single record is
        // decoded: sizeof(DynamicBackendParameters), sizeof(MGPCaps), sizeof(GLFunctionsTable)
        // and the build fingerprint must match, and a mismatch is Fatal{AbiMismatch}.
        MobileGLResult Accept(Transport::ITransport& transport,
                              const std::vector<Uint8>* firstFrame = nullptr);

        // PH-7 (4), ID-P7-3. WHERE A TCP SESSION'S DATA CONNECTION COMES FROM.
        //
        // A Stream data plane is not handed to Accept with the control connection any more: the
        // client opens it after Welcome and names this session by presenting Welcome.dataNonce
        // as its first frame (DataBind). Whoever owns the listener - ServerMain's supervisor,
        // which forwards the descriptor over a socketpair, or the single-session loop, which
        // accepts it directly - reads that frame and offers the connection here with the nonce
        // it presented. Accept compares, binds the one that matches, and refuses every other by
        // name. MOBILEGL_ERR_TIMEOUT from the source means "nothing yet"; anything else ends
        // the wait (TRANSPORT_CLOSED when the control peer went away while it waited).
        using DataConnectionSource =
            std::function<MobileGLResult(std::uint32_t timeoutMs, int* outFd, std::uint8_t* outNonce)>;
        void SetDataConnectionSource(DataConnectionSource source) { m_dataSource = std::move(source); }

        // Re-publishes the whole snapshot. R-12: a SECOND arrival IS the invalidation signal,
        // which is how DirectGLES - which has no OnCapsInvalidated producer - tells the client
        // its InitCapabilities re-ran, without any dev-shaped backend edit. It is also the
        // re-open signal for "the server's ES context died and its rings were dropped", the
        // event MGPipeCallbacks has no eleventh slot for (MGPipeCallbacks.h:56-58).
        MobileGLResult PublishCapsSnapshot();

        Transport::RingConsumer& CommandRing();
        Transport::RingControl& Control();
        Wire::SegmentTable& Segments();
        PipeApplier& Applier();
        ReplyPool& Replies();

        // The client rings this one; the apply thread parks on it.
        // P6 `sm`: the bells a spawn session parks on, injected because they do
        // not come from the transport. CONTRACT-P5 §3.9 kept these accessors on
        // the session precisely so this would be ONE switch in ONE file rather
        // than two virtuals on every transport - and the switch it produced
        // aborts for every role but InProcess, which is where a spawn apply
        // thread died before popping its first record (a6 row A7-1).
        //
        // This is the narrow fix, not the general one: `lk` moves both bells
        // behind ILink and DELETES these accessors, which is what CONTRACT-P6
        // §8.1 means by retiring the ruling rather than amending it. Until then
        // an injected pair is honest about where they come from, and the
        // InProcess arm below is untouched.
        void SetExternalDoorbells(Transport::Doorbell* consumer, Transport::Doorbell* producer);

        Transport::Doorbell& ConsumerDoorbell();
        // The server rings this one, but only when producerParked is set (a store to a shared
        // cache line otherwise burns a big core for a whole frame on a phone).
        Transport::Doorbell& ProducerDoorbell();

        bool HasExternalDoorbells() const { return m_externalConsumerBell != nullptr; }

        // ---- s1's additions beyond c0's signature block ---------------------------------

        // The sizes to create the segments with. Must be called before Accept; after it, the
        // geometry is on the wire in Welcome and changing it would desynchronise the peer.
        void SetSegmentSizes(const Transport::SessionSegmentSizes& sizes);

        // The server role's private BackendObject (ServerLoop::Backend(), v1's). It is NOT
        // pActiveBackendObject - that global holds the client's BackendObject_Remote (table 3).
        // Set before Accept to have the first CapsSnapshot published there; set later and call
        // PublishCapsSnapshot yourself.
        void SetBackend(MG_Backend::BackendObject* backend);

        // MGPCaps::CallMask's two halves. BOTH ARE MANDATORY AND NEITHER HAS A DEFAULT.
        //
        // The first version of this file derived a default for each. That was the phase's
        // marquee defect committed from the server's side: the consumer default read
        // MGPipeGetResourceOps(), a PROCESS-WIDE global (PipeApply.cpp), so under inproc the
        // server answered with whatever the client half of the same process had registered,
        // and under spawn it collapsed to P2's 0x7f. Either way CapsMirror::ServerConsumes
        // then answers a client-side liveness gate with a guess: the client stops emitting
        // five P4a families, CLEARS ITS DIRTY FLAGS ON ACCEPTANCE ANYWAY, and the lane goes
        // green with the uploads lost - ID-39's 66 lost uploads, reflected. R-8's whole point
        // is that a client-side gate may never be answered by a server-side fact; a
        // server-side gate answered by a PROCESS-wide fact is the same defect one level down.
        //
        // So there is no derivation at all. An unset mask is a programming error and
        // CallMask() is a named Fatal on one - loud at the first snapshot instead of silent
        // for a phase. Flagging it in a report was not a mechanism; this is.
        //
        // BITS 0..8, THE MGPCapBit FEATURE BITS. What each one answers belongs to the package
        // that owns the question (kCapTimerQuery to the query family, kCapResidentSubData to
        // b1, and so on). `SetCapabilityBits(0)` is a legitimate and explicit answer - "this
        // server offers no optional capability" - and is the right call while those packages
        // land. kCapNeedsHostIndexBytes / kCapNeedsHostUboBytes must stay 0 for the whole of
        // P5 by ruling (CONTRACT-P5 table 0): they are the only two things that ask for an
        // MGHostSpan, and 0 is what keeps every one of them out of the first IPC frame.
        void SetCapabilityBits(Uint64 capBits);

        // BITS 32..47, THE CONSUMER MASK (R-8 / C-4): which MGPipe subsystems this server has
        // a consumer for. v1 owns the answer - it owns the apply thread and knows what its
        // backend took over. Publishing a bit the server does not consume is the failure
        // above; withholding one the server does consume merely leaves the legacy pull path
        // running, which is the safe direction.
        void SetConsumedSubsystems(Uint64 subsystemMask);

        // False until BOTH setters have been called. A caller that can handle the absence
        // asks this; PublishCapsSnapshot and CallMask abort on it.
        Bool CallMaskIsSet() const;

        // What PublishCapsSnapshot puts on the wire: capBits | MGCapsConsumerBits(subsystems).
        // Fatal{UnsetCallMask} if either half was never set.
        Uint64 CallMask() const;

        Bool Accepted() const;
        // Teardown: after the apply thread has been joined, never before - a record still in
        // flight can still resolve a segment offset (table 3's fourth column).
        void Close();

        Transport::SessionSegments& Shm();
        // The apply loop's end of the rings: WaitForWork / ApplyOne / RetireThrough. This is
        // the only thing that advances appliedSeq, and it advances it by exactly one per
        // record (R-9's ban on batching it while the verb barrier exists).
        Transport::SessionConsumer& Consumer();
        // The reverse channel. P5 only has to be able to CARRY OnBufferWriteback /
        // OnGpuWritten / OnSurfaceChanged; the overflow policy is P9's.
        Transport::EventRingProducer& Events();
        // Publish everything reserved on SEG_EVENT and ring the client. USE THIS rather than
        // EventRingProducer::PublishAndNotify, which takes a bell and a park flag from its
        // caller and therefore compiles for every wrong pairing; the session is the thing
        // that knows which bell belongs to the client.
        void PublishEvents();

        // ---- P5c ev: the reverse channel's producers (CONTRACT-P5C §4) -------------------
        //
        // With an active transport the three reverse MGPipeCallbacks entries are THIS
        // session's producer callbacks (Accept installs them, Close uninstalls them), and
        // the fourth event kind is posted through here: PipeInputs::RecordError's transport
        // arm calls PostGlError with the frontend ErrorCode widened and the ErrorInfo's
        // message. `message` may be null; a longer message than
        // Transport::kEventGlErrorMaxMessageBytes is truncated, which is the contract's
        // ruling (§1), not a check. A session that has not accepted drops the error with a
        // loud line, exactly as RecordError's own no-live-context arm does.
        void PostGlError(Uint32 code, const char* message);
        Transport::ITransport* Control_Plane();

        // ---- PH-6 (ID-P7-2): THE REVERSE CHANNEL IS FORFEITED, NOT FATAL -------------------
        //
        // A run-ahead producer that finds SEG_EVENT full parks until the client drains room
        // (CONTRACT-P5E §2.6). What a client that does not drain used to cost was two Fatals
        // inside ReserveEventOrBlock - "waited 30000 ms", twice, and "a drained ring that still
        // refuses a record it fits is a corrupt cursor set", which a peer that drains one slot at
        // a time reaches in milliseconds. Both are the PEER's behaviour, not a defect of this
        // process, so neither may abort it.
        //
        // What replaces them is a LATCH on this session. The first event that cannot be placed
        // within MOBILEGL_IPC_EVENT_WAIT_MS raises it (ForfeitReverseChannel, which logs
        // `ReverseChannelForfeit{<cause>}` once); that event and every later one is a counted
        // drop (CountDrop on the shared page, ForfeitDrops() here) into scratch the producer may
        // fill and forget; the apply loop's park predicate sees the latch and takes the ordinary
        // stop path; ServerMain's control loop sees the apply thread gone and ends the session.
        // The process exits 0 and the supervisor welcomes the next connection. The latch is
        // set, read and reset on the apply thread's side of Accept/Close only, and read with
        // acquire elsewhere (the control loop, tests).
        Bool ReverseChannelForfeited() const {
            return m_reverseChannelForfeit.load(std::memory_order_acquire);
        }
        // Drops since the latch went up, counted HERE because the shared page's eventDropped is
        // on a page the peer can write.
        Uint64 ForfeitDrops() const { return m_forfeitDrops.load(std::memory_order_acquire); }
        // `cause` is one word (the braces' content); `why` the sentence; `shortDrains` how many
        // times the client cleared the latch without making room. Returns the scratch the caller
        // writes the dropped event into.
        void* ForfeitReverseChannel(const char* cause, const char* why, const char* eventName,
                                    Uint64 payloadBytes, Uint32 waitedMs, Uint32 budgetMs,
                                    Uint32 shortDrains);
        void* DropEventAfterForfeit(Uint64 payloadBytes);

        // PH-6 fix round. SERVERLOOP::STOP'S REQUEST, SEEN BY THE ONE APPLY-THREAD WAIT THAT IS NOT
        // THE LOOP'S OWN PARK. ReserveEventOrBlock parks on the same consumer bell the loop parks
        // on, but with its own predicate, so a Stop() that rang that bell used to wake it only to
        // have it re-test "has the client drained?", find no, and park again for the rest of
        // MOBILEGL_IPC_EVENT_WAIT_MS - while Stop()'s bounded join (5000 ms) ran out and the
        // session died of Fatal{ApplyThreadJoinTimeout}. A control stream that ends (EOF, a
        // malformed frame, a LogFlush ack) while the apply thread waits for a drain is exactly
        // that shape. Stop() raises this BEFORE it rings; the wait's predicate reads it and the
        // reservation forfeits as `Stopped`. Start() lowers it for the next run of the loop.
        void RequestApplyStop() { m_applyStopRequested.store(true, std::memory_order_release); }
        void ClearApplyStopRequest() {
            m_controlStreamEnded.store(false, std::memory_order_release);
            m_applyStopRequested.store(false, std::memory_order_release);
        }
        Bool ApplyStopRequested() const { return m_applyStopRequested.load(std::memory_order_acquire); }
        // WHY the stop came, when the answer is "the peer is gone": ServerMain's control loop
        // notes an EOF or a transport error on the control stream BEFORE it stops the loop, so a
        // reservation that the stop wakes names the hangup (PeerGone) and not the stop. Needed
        // because the stop and the data bell's own death race - on the stream plane the control
        // EOF routinely wins - and a peer killed mid-wait is not a session somebody stopped.
        void NoteControlStreamEnded() { m_controlStreamEnded.store(true, std::memory_order_release); }
        Bool ControlStreamEnded() const { return m_controlStreamEnded.load(std::memory_order_acquire); }

        // completedFrameSerial / presentAckSerial: the two watermarks only the server can
        // advance, kept together with the other three rather than poked into RingControl from
        // whatever code happens to notice a present finished.
        void AdvanceCompletedFrame(Uint64 serial);
        void ReturnPresentCredit(Uint64 serial);

        // Peak-RSS accounting for t1 (RoleMemory.h). `phase` is a short tag.
        Transport::RoleMemorySample SampleMemory() const;
        void LogMemory(const char* phase) const;

    private:
        MobileGLResult BindDataConnection(Transport::ITransport& control, Transport::ILink& link,
                                          const Uint8* nonce);

        DataConnectionSource m_dataSource;
        std::unique_ptr<Transport::ILink> m_link;
        // sm: non-owning; the spawn entry point owns the SocketDoorbells.
        Transport::Doorbell* m_externalConsumerBell = nullptr;
        Transport::Doorbell* m_externalProducerBell = nullptr;
        Transport::RingConsumer* m_commands = nullptr;
        Wire::SegmentTable m_segments;
        PipeApplier m_applier;
        ReplyPool m_replies;

        Transport::SessionConsumer m_consumer;
        Transport::EventRingProducer* m_events = nullptr;
        Transport::ITransport* m_transport = nullptr;
        MG_Backend::BackendObject* m_backend = nullptr;
        Transport::SessionSegmentSizes m_sizes;
        Uint64 m_capBits = 0;
        Uint64 m_consumedSubsystems = 0;
        Bool m_capBitsSet = false;
        Bool m_consumedSet = false;
        Bool m_sizesSet = false;
        Bool m_accepted = false;
        // PH-6. Written by the apply thread (the only SEG_EVENT producer), reset by Accept.
        std::atomic<Bool> m_reverseChannelForfeit{false};
        std::atomic<Uint64> m_forfeitDrops{0};
        std::vector<Uint8> m_forfeitScratch;
        // PH-6 fix round. Written by ServerLoop::Start/Stop and ServerMain's control loop, read by
        // the apply thread's event wait.
        std::atomic<Bool> m_applyStopRequested{false};
        std::atomic<Bool> m_controlStreamEnded{false};
    };

    // Leak-at-exit like every other MG_Remote singleton (ID-8): no frontend destructor may
    // reach pipe or backend state from an exit handler.
    ServerSession& ServerSessionInstance();

} // namespace MobileGL::MG_Remote::Server
