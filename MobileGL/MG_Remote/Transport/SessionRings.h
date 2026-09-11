// MobileGL - MobileGL/MG_Remote/Transport/SessionRings.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ring-owning half of a session: the four segments, the two ring endpoints,
// and the five watermarks. Owner: package s1.
//
// This is the object the P5 gate is really about. ROADMAP.md:21 asks that
// `inproc` run THE SAME G3 codec as `spawn`, and InProcessTransport cannot
// deliver that no matter how it is edited: it is two deque<vector<uint8_t>> plus
// two condvar doorbells (InProcessTransport.cpp:38-97), it owns THE BELLS BUT
// NOT THE RING, and it runs no codec at all. So the rings live here, above
// ITransport, in one implementation both delivery modes use - and the transport
// supplies the control plane and the two bells, which is exactly what its own
// header says it is for (ITransport.h:16-20: "everything on the hot path
// bypasses this interface entirely").
//
// INPROC USES ShmSegment TOO, NOT new[]. In one address space a heap allocation
// would work and would be faster to write. It is refused deliberately: it is half
// of what makes "the same code path" true rather than nominal. A `new` here means
// the mapping, the alignment, the size rounding, the read-only peer view and the
// lifetime are all exercised for the first time in P6, on the day the second
// process appears - which is the shape of every "it was green in CI" failure this
// phase is trying not to repeat.
//
// ---------------------------------------------------------------------------
// THE RING CAPACITY IS HALF THE SEGMENT, AND THAT IS ARITHMETIC, NOT A CHOICE.
//
// Ring.h:11-13 puts RingControl at the HEAD of SEG_CMD, and RingProducer requires
// a POWER-OF-TWO capacity (Ring.cpp:89-103, the mask is the indexing). A segment
// of 8 MiB therefore has 8 MiB - 4096 bytes left for records, and the largest
// power of two that fits is 4 MiB. A record may be at most half the ring
// (RingProducer::MaxRecordBytes), so the real cap on one record is 2 MiB.
//
// CONTRACT-P5 §5 and Config.h's MOBILEGL_IPC_RING_MB comment both say "8 MiB caps
// one record at 4 MiB". That arithmetic assumed the whole segment is ring bytes
// and did not subtract the control page. The number here is HALF of theirs, and
// the deviation is deliberately in the SAFE direction: R-10's obligation is to
// PROVE no record ever approaches the cap, and a lower cap makes that proof fire
// earlier and louder rather than later and silently. The alternatives were both
// worse - announcing SegmentRef.sizeBytes as 4096 + 8 MiB breaks the four sizes
// ProtocolSmokeTest.cpp:72 pins, and moving RingControl out of SEG_CMD needs a
// fifth SegmentRef that Welcome does not have.
//
// SEG_STAGE has no control page of its own: RingControl carries TWO cursor
// triples (Ring.h:101-109) and the stage triple is the second. So SEG_STAGE's
// capacity is its whole segment, and 32 MiB is already a power of two.
// ---------------------------------------------------------------------------

#pragma once

#include "Doorbell.h"
#include "EventRing.h"
#include "ReplySlot.h"
#include "Ring.h"
#include "RoleMemory.h"
#include "ShmSegment.h"

#include <atomic>
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    // The four sizes are CONTRACT-P5's and are pinned by ProtocolSmokeTest.cpp:72.
    // MOBILEGL_IPC_RING_MB / MOBILEGL_IPC_STAGE_MB move the first two.
    struct SessionSegmentSizes {
        std::uint64_t CmdBytes = 8ull * 1024 * 1024;
        std::uint64_t StageBytes = 32ull * 1024 * 1024;
        std::uint64_t ReplyBytes = 8ull * 1024 * 1024;
        std::uint64_t EventBytes = 256ull * 1024;
        std::uint32_t ReplySlotCount = kDefaultReplySlotCount;
    };

    // Largest power of two <= `bytes`, or 0 when there is none. The ring's
    // indexing is a mask, so this is what any segment's usable ring area is.
    std::uint64_t LargestPowerOfTwoAtMost(std::uint64_t bytes);

    // Usable ring capacity of a segment that carries a RingControl page at its
    // head. See the header block above for why this is half the segment.
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

        // Creates and maps all four, initialises BOTH control pages (SEG_CMD's
        // and SEG_EVENT's), and books the mapping in `role`'s ledger.
        MobileGLResult Create(const SessionSegmentSizes& sizes, MemoryRole role);

        // The inproc peer's view: the SAME mapping, booked under the OTHER role.
        // It does not re-init the control pages - there is one shared page and
        // re-initialising it would zero the owner's cursors under it.
        MobileGLResult AttachInProcess(SessionSegments& owner, MemoryRole role);

        void Close();
        bool Valid() const { return m_valid; }

        RingControl* CmdControl() const { return m_cmdControl; }
        void* CmdRingBase() const { return m_cmdRingBase; }
        std::uint64_t CmdRingCapacity() const { return m_cmdRingCapacity; }

        void* StageBase() const { return m_stageBase; }
        std::uint64_t StageCapacity() const { return m_stageCapacity; }

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

        ShmSegment m_owned[4]; // empty on an attached (peer) view
        ShmSegment* m_segments[4] = {nullptr, nullptr, nullptr, nullptr};

        RingControl* m_cmdControl = nullptr;
        void* m_cmdRingBase = nullptr;
        std::uint64_t m_cmdRingCapacity = 0;
        void* m_stageBase = nullptr;
        std::uint64_t m_stageCapacity = 0;
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
    // The five watermarks (R-9). Every write and every wait goes through here,
    // so the rules in Ring.h's header have exactly one implementation.
    // -----------------------------------------------------------------------
    //
    // THE ONE RULE THAT MATTERS: a watermark may be published LATE but NEVER
    // EARLY. Late costs a waiter some latency; early makes every waiter a silent
    // use of work that has not happened, and there is no checksum anywhere on
    // this ring that would catch it. So the advances below REFUSE to move a
    // watermark backwards (that is the detectable half) and the callers are
    // responsible for never calling them before the work is done (that is the
    // half only a call-site review and R-9's unit cases can enforce).
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
        // can un-park a waiter on kWaitForever (Doorbell.h:211-221), and the
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
        void Attach(RingControl* control, RingProducer* cmd, RingProducer* stage, Doorbell* peerBell,
                    Doorbell* selfBell, std::uint32_t spinUs);
        void Detach();
        bool Valid() const { return m_control != nullptr && m_cmd != nullptr; }

        // Publish the command ring's head, record submittedSeq, THEN ring - in
        // that order and never any other. Doorbell.h:186-193: the fence only
        // orders what precedes it, so ringing before publishing reopens the very
        // lost-wakeup window the fences exist to close. RingTest.cpp:446 pins the
        // call order; this is the one place production code performs it.
        void PublishAndNotify(std::uint64_t submittedSeq);

        // The verb barrier's wait, AND the reply's wait: they are the same wait
        // (R-3/R-5), which is why a blocking ReadPixels, MapPersistent's decline
        // and the four Bool acceptances cost ZERO extra round trips.
        SessionWait WaitForApplied(std::uint64_t seq, std::uint32_t timeoutMs);
        // Present throttle.
        SessionWait WaitForPresentAck(std::uint64_t serial, std::uint32_t timeoutMs);
        // Back-pressure when Reserve returned nullptr. NEVER call this when
        // FreeBytes() is already >= the record: Ring.h:226-233 - a nullptr with
        // enough free bytes can only mean "too big, chunk", and waiting on it
        // stalls forever.
        SessionWait WaitForCmdSpace(std::uint64_t bytes, std::uint32_t timeoutMs);
        SessionWait WaitForStageSpace(std::uint64_t bytes, std::uint32_t timeoutMs);

        RingControl* Control() const { return m_control; }
        RingProducer* Cmd() const { return m_cmd; }
        RingProducer* Stage() const { return m_stage; }
        Doorbell* PeerDoorbell() const { return m_peerBell; }
        Doorbell* SelfDoorbell() const { return m_selfBell; }
        std::uint32_t SpinUs() const { return m_spinUs; }

    private:
        template <class Ready>
        SessionWait Park(Ready&& ready, std::uint32_t timeoutMs);

        RingControl* m_control = nullptr;
        RingProducer* m_cmd = nullptr;
        RingProducer* m_stage = nullptr;
        Doorbell* m_peerBell = nullptr;
        Doorbell* m_selfBell = nullptr;
        std::uint32_t m_spinUs = kDefaultSpinUs;
    };

    // -----------------------------------------------------------------------
    // The server's end of the rings: the apply thread's loop, minus the applier.
    // -----------------------------------------------------------------------
    class SessionConsumer {
    public:
        SessionConsumer() = default;

        void Attach(RingControl* control, RingConsumer* cmd, Doorbell* peerBell, Doorbell* selfBell,
                    std::uint32_t spinUs);
        void Detach();
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
            apply(view);
            ++m_appliedSeq;
            Watermark::AdvanceApplied(*m_control, m_appliedSeq);
            m_cmd->PublishApplied();
            NotifyClient();
            return true;
        }

        // Records without kRecBorrowSlot retire as soon as they are applied; a
        // borrowed slot retires on completedFrameSerial, which is why this is a
        // separate call and not folded into ApplyOne.
        void RetireThrough(std::uint64_t seq);

        // Ring the client's bell, but only when it said it is parked: a store to
        // a shared cache line otherwise burns a big core for a whole frame on a
        // phone (Doorbell.h:13-22).
        void NotifyClient();

        std::uint64_t AppliedSeq() const { return m_appliedSeq; }
        RingControl* Control() const { return m_control; }
        RingConsumer* Cmd() const { return m_cmd; }
        Doorbell* PeerDoorbell() const { return m_peerBell; }
        Doorbell* SelfDoorbell() const { return m_selfBell; }
        std::uint32_t SpinUs() const { return m_spinUs; }

    private:
        RingControl* m_control = nullptr;
        RingConsumer* m_cmd = nullptr;
        Doorbell* m_peerBell = nullptr;
        Doorbell* m_selfBell = nullptr;
        std::uint32_t m_spinUs = kDefaultSpinUs;
        std::uint64_t m_appliedSeq = 0;
    };

    // -----------------------------------------------------------------------
    // The ABI fingerprint's mixer.
    //
    // It lives under Transport/ rather than in CapsCodec.cpp so that it can be
    // tested without the GL frontend's umbrella header, and so that the SIZES it
    // mixes are the caller's - CapsCodec.cpp passes the three real sizeofs, a
    // unit test passes made-up ones and can then prove a one-byte difference
    // changes the answer. A fingerprint that cannot be shown to change is
    // indistinguishable from one that is never compared.
    // -----------------------------------------------------------------------
    std::uint64_t MixAbiFingerprint(std::uint64_t dynamicParamsSize, std::uint64_t capsSize,
                                    std::uint64_t functionTableSize, std::uint32_t abiVersion,
                                    const char* buildStamp);

} // namespace MobileGL::MG_Remote::Transport
