// MobileGL - MobileGL/MG_Remote/Transport/SessionRings.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ring-owning half of a session: the four segments, the two ring endpoints, the five
// watermarks. The rings live above ITransport (control plane only), so every delivery mode runs
// the same ring and codec code, and `inproc` maps real ShmSegments rather than heap memory so the
// mapping, sizing and peer-view paths are exercised on every run.
// Sizes name the ring: SEG_CMD and SEG_EVENT are a power-of-two ring plus one RingControl page at
// the head, so MOBILEGL_IPC_RING_MB (rounded down to a power of two) is the command ring itself
// and MaxRecordBytes() is half of it; SegmentRef.sizeBytes announces the whole mapping. SEG_STAGE
// (a linear allocator) and SEG_REPLY (a slot pool) have no control page.

#pragma once

#include "Doorbell.h"
#include "EventRing.h"
#include "ReplySlot.h"
#include "Ring.h"
#include "RoleMemory.h"
#include "ShmSegment.h"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace MobileGL::MG_Remote::Transport {

    // RING sizes, not segment sizes - see the header block. The four defaults are
    // CONTRACT-P5's; MOBILEGL_IPC_RING_MB / MOBILEGL_IPC_STAGE_MB move the first
    // two, and ServerSession applies them unless SetSegmentSizes overrode them.
    struct SessionSegmentSizes {
        std::uint64_t CmdRingBytes = 8ull * 1024 * 1024; // + one control page
        // SEG_STAGE IS NOT A RING. Package w1's encoder owns it as an
        // encoder-local LINEAR ALLOCATOR that reclaims on retiredSeq, so this is
        // a plain byte count: no control page, and no rounding down to a power of
        // two either. Rounding was a ring requirement and keeping it would have
        // silently turned an operator's MOBILEGL_IPC_STAGE_MB=24 into 16.
        std::uint64_t StageBytes = 32ull * 1024 * 1024;
        // SEG_REPLY: a slot pool, not a ring, and NO KNOB MOVES IT (contract §5 has
        // none). ID-47: 16 MiB = eight slots of 2 MiB, sized from the largest P5
        // read - the E2 retrace's full-surface 640x480 RGBA8 snapshot, 1,228,800
        // bytes - which the previous 8 MiB / 1 MiB-per-slot pool could not hold
        // (ReplySlot.h says how it was found). ProtocolSmokeTest pins the number.
        std::uint64_t ReplyBytes = 16ull * 1024 * 1024;
        std::uint64_t EventRingBytes = 256ull * 1024; // + one control page
        std::uint32_t ReplySlotCount = kDefaultReplySlotCount;
    };

    // Largest power of two <= `bytes`, or 0 when there is none. The ring's
    // indexing is a mask, so this is what a ring capacity has to be rounded to.
    std::uint64_t LargestPowerOfTwoAtMost(std::uint64_t bytes);

    // How big a segment has to be to hold `ringBytes` of ring behind its control
    // page. `ringBytes` is rounded DOWN to a power of two first, so an operator
    // who asks for 6 MiB gets a 4 MiB ring in a 4 MiB + 4096 segment rather than
    // a segment whose tail can never be addressed.
    std::uint64_t SegmentBytesForRing(std::uint64_t ringBytes);

    // The usable ring inside a segment that carries a RingControl page at its
    // head. The inverse of SegmentBytesForRing for any size it produced.
    std::uint64_t RingCapacityForSegment(std::uint64_t segmentBytes);

    enum class SessionSegmentSlot : std::uint32_t {
        Cmd = 0,
        Stage = 1,
        Reply = 2,
        Event = 3,
        kSessionSegmentCount = 4,
    };

    // The four ShmSegments of one session, created and mapped read/write.
    //
    // WHO CREATES THEM: the SERVER, because Welcome announces all four
    // (protocol.fbs's Welcome table) and Welcome is server -> client. "Client
    // owned" in the schema's comments is about who WRITES a segment, not who
    // allocates it. Under `inproc` the client then attaches to the same mapping
    // (AttachInProcess); under `spawn` it will adopt the fds the server passed by
    // SCM_RIGHTS, which is P6's and is why Adopt is on ShmSegment already.
    class SessionSegments {
    public:
        SessionSegments() = default;
        ~SessionSegments();

        SessionSegments(const SessionSegments&) = delete;
        SessionSegments& operator=(const SessionSegments&) = delete;

        // Creates and maps all four, initializes BOTH control pages (SEG_CMD's
        // and SEG_EVENT's), and books the mapping in `role`'s ledger.
        MobileGLResult Create(const SessionSegmentSizes& sizes, MemoryRole role);
        // Private, identically addressed mirrors for the stream data plane.
        MobileGLResult CreatePrivate(const SessionSegmentSizes& sizes, MemoryRole role);

        // The inproc peer's view of the owner's four segments, booked under the other role. It
        // does not re-initialize the control pages: they are shared, and that would zero the
        // owner's cursors. On POSIX each descriptor is dup()ed and adopted (ShmSegment::Adopt +
        // Map), a real second mapping, so the inproc attach runs the same path as the cross-process
        // one (AdoptFromDescriptors) and the two views close independently. Windows has no Adopt
        // (the section name travels in SegmentRef), so there the view aliases the owner's.
        MobileGLResult AttachInProcess(SessionSegments& owner, MemoryRole role);

        // P6 `sm`: the spawn client's half. The four descriptors arrived over
        // SCM_RIGHTS and this side adopts and maps them.
        //
        // IT IS THE SAME THREE CALLS AttachInProcess ALREADY MAKES - Adopt, then
        // Map, per slot - because that was the point of writing the inproc attach
        // as dup+Adopt+Map rather than as an alias (see the comment above it).
        // The fstat size check inside Adopt, the alignment, the peer lifetime and
        // the ledger booking have all been exercised on every inproc run since
        // P5; what is new here is only where the fd came from.
        //
        // OWNERSHIP: on success this object owns all four descriptors and closes
        // them in Close(). On failure it closes none of them - the caller still
        // owns what it received and is the only one that can report which slot
        // failed, so a half-consuming failure path would lose descriptors in the
        // one situation where the diagnostic matters.
        MobileGLResult AdoptFromDescriptors(const int fds[4], const std::uint64_t sizes[4],
                                            std::uint32_t replySlotCount, MemoryRole role);

        void Close();
        bool Valid() const { return m_valid; }

        RingControl* CmdControl() const { return m_cmdControl; }
        void* CmdRingBase() const { return m_cmdRingBase; }
        std::uint64_t CmdRingCapacity() const { return m_cmdRingCapacity; }

        // The whole SEG_STAGE mapping, for w1's linear allocator and for the
        // SegmentTable view the decoder resolves blobrefs against. There is no
        // stage RING and no RingProducer/RingConsumer over RingCursorSet::Stage.
        void* StageBase() const { return m_stageBase; }
        std::uint64_t StageBytes() const { return m_stageBytes; }

        void* ReplyBase() const { return m_replyBase; }
        std::uint64_t ReplyBytes() const { return m_replyBytes; }
        std::uint32_t ReplySlotCount() const { return m_replySlotCount; }

        RingControl* EventControl() const { return m_eventControl; }
        void* EventSegmentBase() const { return m_eventSegmentBase; }
        void* EventRingBase() const { return m_eventRingBase; }
        std::uint64_t EventRingCapacity() const { return m_eventRingCapacity; }

        // For Welcome's four SegmentRefs. The announced size is the MAPPING size,
        // which is what a peer must map - not the ring capacity inside it.
        std::uint64_t AnnouncedSize(SessionSegmentSlot slot) const;
        const char* AnnouncedName(SessionSegmentSlot slot) const;
        int DescriptorFor(SessionSegmentSlot slot) const; // POSIX; -1 elsewhere

        std::uint64_t MappedBytes() const { return m_mappedBytes; }

    private:
        void DeriveViews();

        void* m_private[4] = {};
        std::uint64_t m_privateSizes[4] = {};
        ShmSegment m_owned[4]; // empty on an attached (peer) view
        ShmSegment* m_segments[4] = {nullptr, nullptr, nullptr, nullptr};

        RingControl* m_cmdControl = nullptr;
        void* m_cmdRingBase = nullptr;
        std::uint64_t m_cmdRingCapacity = 0;
        void* m_stageBase = nullptr;
        std::uint64_t m_stageBytes = 0;
        void* m_replyBase = nullptr;
        std::uint64_t m_replyBytes = 0;
        std::uint32_t m_replySlotCount = kDefaultReplySlotCount;
        RingControl* m_eventControl = nullptr;
        void* m_eventSegmentBase = nullptr;
        void* m_eventRingBase = nullptr;
        std::uint64_t m_eventRingCapacity = 0;

        std::uint64_t m_mappedBytes = 0;
        MemoryRole m_role = MemoryRole::Client;
        bool m_valid = false;
        bool m_owns = false;
        bool m_booked = false;
    };

    // -----------------------------------------------------------------------
    // The five watermarks. Every write and every wait goes through here, so the rules in
    // Ring.h's header have exactly one implementation.
    // -----------------------------------------------------------------------
    // A watermark may be published late but never early: early lets waiters use work that has not
    // happened, and nothing on the ring would catch it, so callers advance only after the work is
    // done. A watermark moving backwards is Fatal{ProtocolCorruption}: a stale shared appliedSeq
    // would make every later advance a no-op and every WaitForApplied(kWaitForever) hang.
    namespace Watermark {

        // Producer, after Publish. Nobody waits on it - it is the answer to "how
        // far ahead of the server is the client right now".
        void AdvanceSubmitted(RingControl& control, std::uint64_t seq);
        // Consumer, ONCE PER APPLIED RECORD. P5 forbids the 64-record batching
        // this ring was designed for: the verb barrier and every reply wait read
        // it. kRecPad does not count - RingConsumer::Pop skips fillers, so the
        // rule is kept by counting Pops rather than bytes.
        void AdvanceApplied(RingControl& control, std::uint64_t seq);
        // Consumer, once the SEG_STAGE bytes a record referenced are finished
        // with. The staging allocator reclaims behind it and nothing else may.
        void AdvanceRetired(RingControl& control, std::uint64_t seq);
        // Server, when a present completes. Trails appliedSeq by the GPU's own
        // depth; never conflate the two.
        void AdvanceCompletedFrame(RingControl& control, std::uint64_t serial);
        // Server, when it returns a present credit. The only back-pressure that
        // bounds latency rather than bytes.
        void AdvancePresentAck(RingControl& control, std::uint64_t serial);

        // Every wait is >=, never ==: both sides advance in jumps, and an
        // equality waiter misses its wakeup and hangs until the next coincidence.
        inline bool Reached(const std::atomic<std::uint64_t>& watermark, std::uint64_t target) {
            return watermark.load(std::memory_order_acquire) >= target;
        }

    } // namespace Watermark

    enum class SessionWait : std::uint32_t {
        Reached = 0,
        // The doorbell died: the peer shut the session down. The ONLY thing that
        // can un-park a waiter on kWaitForever (CondVarDoorbell::Kill), and the
        // reason a bounded join is possible at all.
        ShutDown = 1,
        TimedOut = 2,
    };

    // -----------------------------------------------------------------------
    // The client's end of the rings.
    //
    // THE TWO DOORBELL ACCESSORS LIVE ON THE SESSION, NOT ON ITransport
    // (contract §3.9, the ruling s1 is asked to make now rather than let P6
    // discover). The session takes the two references InProcessTransport hands
    // out and is the only thing that knows which is which; ITransport stays the
    // dumb control-plane interface its header claims to be, and P6's
    // SocketTransport does not grow two accessors it has no natural home for.
    // -----------------------------------------------------------------------
    class SessionProducer {
    public:
        SessionProducer() = default;

        // `peerBell` is the bell the SERVER parks on and this side rings;
        // `selfBell` is this side's own. InProcessTransport::PeerDoorbell() and
        // SelfDoorbell() are exactly that pair, from the client endpoint.
        // NO STAGE RING. SEG_STAGE is w1's encoder-local linear allocator and
        // RingCursorSet::Stage has no producer and no consumer in P5 - see
        // RingControl's stage triple in Ring.h. A producer here would publish
        // stageHead with nothing advancing the two tails, so FreeBytes() would
        // fall to zero the first time the head lapped the capacity and never
        // recover: a guaranteed hang, not a slow path.
        void Attach(ILink& link, std::uint32_t spinUs);
        void Attach(RingControl* control, RingProducer* cmd, Doorbell* peerBell, Doorbell* selfBell,
                    std::uint32_t spinUs);
        void Detach();
        void SetLink(ILink* link) { m_link = link; }
        bool Valid() const { return m_control != nullptr && m_cmd != nullptr; }

        // Publish the command ring's head, record submittedSeq, THEN ring - in
        // that order and never any other. NotifyIfParked's PRECONDITION: the fence only
        // orders what precedes it, so ringing before publishing reopens the very
        // lost-wakeup window the fences exist to close. RingTest.cpp:446 pins the
        // call order; this is the one place production code performs it.
        void PublishAndNotify(std::uint64_t submittedSeq);

        // The last seq THIS producer published, kept locally rather than read back
        // out of RingControl::submittedSeq. Teardown's drain needs it: ARCHITECTURE.md §11.2.1
        // explicitly permits submittedSeq to be published LAZILY and Ring.h's RingProducer::Publish
        // encourages batching the publish, so the shared watermark may lag the
        // emitter - and a drain that waits for `appliedSeq >= submittedSeq` would
        // then under-wait and free an emitter's var-tail while a record still
        // names it. Run-ahead and the batched waits publish without waiting, so
        // at teardown the two may differ.
        std::uint64_t LastPublishedSeq() const { return m_lastPublishedSeq; }

        // The verb barrier's wait, AND the reply's wait: they are the same wait
        // (R-3/R-5), which is why a blocking ReadPixels, MapPersistent's decline
        // and the four Bool acceptances cost ZERO extra round trips.
        SessionWait WaitForApplied(std::uint64_t seq, std::uint32_t timeoutMs);
        // Present throttle.
        SessionWait WaitForPresentAck(std::uint64_t serial, std::uint32_t timeoutMs);

        // ---- P5e (ra), CONTRACT-P5E §2.6: the same two waits, PLUS the event ring ---------
        //
        // A parked client must be wakeable by a server that ran out of SEG_EVENT, or the
        // flow-control deadlock is one burst wide: the server stops producing, so it stops
        // applying, so appliedSeq never reaches the seq this waiter is parked on, so the ring
        // is never drained. Both predicates therefore break on `eventRingFull` as well, and
        // the CALLER re-reads the watermark to find out which of the two woke it - a third
        // SessionWait value would have made every existing `== Reached` site a bug.
        //
        // The old entry points above are unchanged and keep their callers: teardown's
        // WaitForApplied has no ring to drain and must not learn about one.
        SessionWait WaitForAppliedOrEventBacklog(std::uint64_t seq, std::uint32_t timeoutMs);
        SessionWait WaitForPresentAckOrEventBacklog(std::uint64_t serial, std::uint32_t timeoutMs);
        // True when the peer latched "SEG_EVENT is full and I stopped producing".
        bool EventRingIsFull() const;
        // Back-pressure when Reserve returned nullptr. NEVER call this when
        // FreeBytes() is already >= the record: RingProducer::Reserve (Ring.h) - a nullptr with
        // enough free bytes can only mean "too big, chunk", and waiting on it
        // stalls forever.
        SessionWait WaitForCmdSpace(std::uint64_t bytes, std::uint32_t timeoutMs);

        LinkProgress* Progress() const { return m_control ? &m_control->Progress : nullptr; }
        RingControl* Control() const { return m_control; }
        RingProducer* Cmd() const { return m_cmd; }
        Doorbell* PeerDoorbell() const { return m_peerBell; }
        Doorbell* SelfDoorbell() const { return m_selfBell; }
        std::uint32_t SpinUs() const { return m_spinUs; }

        // The client half of the wait ledger. Waits() counts every entry into Park (the verb
        // barrier, the present throttle, the ring-space wait); Parks() counts those that ran out of
        // spin budget and really blocked. Many waits with few parks means the server nearly keeps
        // up; many parks means it does not. Parks is fed through Wait's `parkTally`, not read from
        // the self bell's ParkEntries(): that bell also carries the encoder's SEG_STAGE retirement
        // wait (spinUs 0, so it always parks), which would let parks exceed waits.
        std::uint64_t Waits() const { return m_waits.load(std::memory_order_relaxed); }
        std::uint64_t Parks() const { return m_parks.load(std::memory_order_relaxed); }

    private:
        template <class Ready>
        SessionWait Park(Ready&& ready, std::uint32_t timeoutMs);

        ILink* m_link = nullptr;
        RingControl* m_control = nullptr;
        RingProducer* m_cmd = nullptr;
        Doorbell* m_peerBell = nullptr;
        Doorbell* m_selfBell = nullptr;
        std::uint32_t m_spinUs = kDefaultSpinUs;
        std::uint64_t m_lastPublishedSeq = 0;
        // Relaxed atomics, not plain integers: the GL thread owns this producer, but
        // ClientSession::Stop's bounded drain waits through it from whichever thread is tearing
        // the session down, and a diagnostic is not worth a data race. Relaxed costs the same
        // as a plain add on both architectures the split runs on. m_parks is incremented by
        // Doorbell::Wait through the `parkTally` pointer, once per wait that really blocked.
        std::atomic<std::uint64_t> m_waits{0};
        std::atomic<std::uint64_t> m_parks{0};
    };

    // -----------------------------------------------------------------------
    // The server's end of the rings: the apply thread's loop, minus the applier.
    // -----------------------------------------------------------------------
    class SessionConsumer {
    public:
        SessionConsumer() = default;

        void Attach(ILink& link, std::uint32_t spinUs);
        void Attach(RingControl* control, RingConsumer* cmd, Doorbell* peerBell, Doorbell* selfBell,
                    std::uint32_t spinUs);
        void Detach();
        void SetLink(ILink* link) { m_link = link; }
        bool Valid() const { return m_control != nullptr && m_cmd != nullptr; }

        // Park until a record is waiting, the session is shut down, or the
        // deadline passes. Pass kWaitForever for the steady state; a dead bell is
        // what ends it, which is why Doorbell::Kill() is load-bearing for the
        // join (InProcessTransportTest.cpp:344 pins the shape).
        SessionWait WaitForWork(std::uint32_t timeoutMs);

        // Pop ONE record and hand it to `apply`. Returns false when the ring is
        // empty. On a corrupt header it returns false and sets *outCorrupt, which
        // the CALLER escalates to Fatal{ProtocolCorruption} rather than retrying.
        //
        // This is the only place appliedSeq is advanced, and it advances it by
        // EXACTLY ONE per record - never a batch (R-9). kRecPad cannot reach
        // `apply`: RingConsumer::Pop skips fillers before returning, so a filler
        // is never counted here and the two sides' sequence spaces cannot drift.
        // Order: apply -> appliedSeq -> PublishApplied -> ring the client.
        template <class Apply>
        bool ApplyOne(Apply&& apply, bool* outCorrupt = nullptr) {
            if (outCorrupt != nullptr) {
                *outCorrupt = false;
            }
            if (!Valid()) {
                return false;
            }
            RingRecordView view{};
            if (!m_cmd->Pop(view, outCorrupt)) {
                return false;
            }
            // A terminal rejection after Pop must not acknowledge an unapplied record.
            // A bool callback returning false ends this consumer's session; it cannot retry
            // the popped record. Existing void callbacks retain their original semantics.
            // ProducingSeq() names THIS record while its apply runs, so a reverse
            // event the apply posts carries the seq of the record that produced it.
            struct ApplyingScope {
                bool& flag;
                explicit ApplyingScope(bool& f) : flag(f) { flag = true; }
                ~ApplyingScope() { flag = false; }
            };
            {
                const ApplyingScope applying(m_applying);
                if constexpr (std::is_same_v<std::invoke_result_t<Apply, const RingRecordView&>, bool>) {
                    if (!apply(view)) return false;
                } else {
                    apply(view);
                }
            }
            ++m_appliedSeq;
            Watermark::AdvanceApplied(*m_control, m_appliedSeq);
            m_cmd->PublishApplied();
            // THE BYTE CURSOR RetireThrough MAY RECLAIM TO, which is NOT simply
            // "everything popped". A record carrying kRecBorrowSlot has been lent
            // into the GPU timeline and its slot can only be recycled after
            // completedFrameSerial (Ring.h's header comment), so the reclaimable cursor stops
            // AT the first borrowed record and does not move again until
            // RetireBorrowedUpTo releases it. Nothing sets kRecBorrowSlot yet;
            // this is here so that the day something does, the producer does not
            // overwrite a slot the GPU is still reading.
            if ((view.flags & kRecBorrowSlot) == 0 && !m_borrowHeld) {
                m_retirableCursor = view.cursor + sizeof(RingRecordHeader) + view.payloadSize;
            } else {
                if ((view.flags & kRecBorrowSlot) != 0) {
                    // P5 PRODUCES NO BORROWED SLOTS AT ALL, so this bit arriving
                    // is either a borrow nobody implemented or MGPipeCallFlags::
                    // kHostSpan wearing kRecBorrowSlot's bit (Ring.h's collision
                    // table) - and P5's reduced path is ruled to produce zero
                    // host spans too. Either way the conservative arm is taken
                    // (nothing past it is reclaimed) and the sighting is NAMED,
                    // because the alternative is a producer that wedges on the
                    // first full ring with no line anywhere saying why.
                    NoteBorrowedRecord(view.kind, view.flags);
                }
                m_borrowHeld = true;
            }
            NotifyClient();
            return true;
        }

        // Publish the retire watermark and hand back every byte up to the first
        // still-borrowed record.
        //
        // IT IS MANDATORY, NOT OPTIONAL. RingProducer::FreeBytes() reclaims against
        // retiredTail ONLY (Ring.cpp:110-115) and nothing else in this class
        // publishes it, so an apply loop that calls ApplyOne and never this wedges
        // the producer on the first full ring. Call it once per drain batch.
        void RetireThrough(std::uint64_t seq);

        // Release borrowed slots up to `cursor` once completedFrameSerial has
        // passed them. `cursor` is a RingRecordView::cursor the apply loop kept.
        // This is the only thing that moves the reclaim point past a borrowed
        // record - see ApplyOne.
        void RetireBorrowedUpTo(std::uint64_t cursor);

        // The byte cursor RetireThrough would reclaim to right now. Diagnostic;
        // a borrow that is never released shows up as this number standing still.
        std::uint64_t RetirableCursor() const { return m_retirableCursor; }

        // completedFrameSerial and presentAckSerial, advanced AND rung. The free
        // functions in namespace Watermark advance only: a v1 caller that used one
        // directly would leave a client parked in WaitForPresentAck(kWaitForever)
        // with nothing to wake it, because the advance and the doorbell are two
        // separate stores and only the pair is a wakeup.
        void CompleteFrame(std::uint64_t serial);
        void ReturnPresentCredit(std::uint64_t serial);

        // Ring the client's bell, but only when it said it is parked: a store to
        // a shared cache line otherwise burns a big core for a whole frame on a
        // phone (Doorbell.h's file header, the bidirectional argument).
        void NotifyClient();

        std::uint64_t AppliedSeq() const { return m_appliedSeq; }
        // The seq of the record whose apply is running, or - between records -
        // the last one applied. Every byte the server reads back at this point is
        // current as of exactly this record, which is what a writeback event
        // stamps so the client can order it against its own later records.
        std::uint64_t ProducingSeq() const { return m_applying ? m_appliedSeq + 1 : m_appliedSeq; }
        // How many kRecBorrowSlot records this consumer has seen. Non-zero in P5
        // is a finding, not a statistic.
        std::uint64_t BorrowedRecordsSeen() const { return m_borrowedSeen; }
        LinkProgress* Progress() const { return m_control ? &m_control->Progress : nullptr; }
        RingControl* Control() const { return m_control; }
        RingConsumer* Cmd() const { return m_cmd; }
        Doorbell* PeerDoorbell() const { return m_peerBell; }
        Doorbell* SelfDoorbell() const { return m_selfBell; }
        std::uint32_t SpinUs() const { return m_spinUs; }

    private:
        ILink* m_link = nullptr;
        RingControl* m_control = nullptr;
        RingConsumer* m_cmd = nullptr;
        Doorbell* m_peerBell = nullptr;
        Doorbell* m_selfBell = nullptr;
        std::uint32_t m_spinUs = kDefaultSpinUs;
        std::uint64_t m_appliedSeq = 0;
        std::uint64_t m_retirableCursor = 0;
        std::uint64_t m_borrowedSeen = 0;
        bool m_borrowHeld = false;
        bool m_applying = false;

        // Out of line so ApplyOne, which is a template in a header this layer
        // keeps free of MobileGL/Includes.h, can still log.
        void NoteBorrowedRecord(std::uint16_t kind, std::uint16_t flags);
    };

    // -----------------------------------------------------------------------
    // P6.5 wire fingerprint: one mixer, driven by compiler-derived layout facts.
    // CapsCodec supplies generated member/catalog digests; build identity and
    // negotiated window sizes do not participate in wire compatibility.
    // -----------------------------------------------------------------------
    // Wire facts only. Segment geometry and build identity are negotiated separately.
    struct AbiFingerprintInputs {
        std::uint64_t DynamicParamsSize = 0;
        std::uint64_t CapsSize = 0;
        std::uint64_t MemberLayout = 0;
        std::uint64_t CatalogueLayout = 0;
        std::uint64_t RenderStateLayout = 0;
        std::uint64_t FormatCapabilityTargets = 0;
        std::uint64_t FormatCapabilityFormats = 0;
        std::uint64_t FormatCapabilitiesCodecVersion = 0;
        std::uint64_t RendererInfoCodecVersion = 0;
        std::uint64_t ProgramArtifactsCodecVersion = 0;
        std::uint64_t ProgramArtifactsSchema = 0;
        std::uint64_t OpCount = 0;
        // MOBILEGL_PROTOCOL_CONTROL_REVISION (mg_protocol_base.h): the control schema's shape.
        std::uint64_t ControlSchemaRevision = 0;
        std::uint32_t AbiVersion = 0;
        std::uint32_t PointerBits = 0;
        std::uint32_t LittleEndian = 0;
    };

    // FNV-1a over every field above, in declaration order. Never 0: that value is
    // reserved for "not stated", so a peer that forgot to fill the field cannot
    // accidentally agree with one that did.
    std::uint64_t MixAbiFingerprint(const AbiFingerprintInputs& inputs);

} // namespace MobileGL::MG_Remote::Transport
